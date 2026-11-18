#include "port_internal.h"
#include "record_io.h"
#include "pokewalker_host.h"
#include "project.h"
#include "application/pw_eeprom_m95512.h"
#include "eeprom_map.h"
#include "eeprom_address.h"
#include "score.h"
#include <string.h>

static u16 swap16(u16 value)
{
  return __builtin_bswap16(value);
}

static u32 swap32(u32 value)
{
  return __builtin_bswap32(value);
}

void pw_status_swap(DeviceStatus *record)
{
  record->rtcSeconds = swap32(record->rtcSeconds);
  record->totalSteps = swap32(record->totalSteps);
}

static void swap_save(void *bytes)
{
  SaveData *record = bytes;
  record->totalSteps = swap32(record->totalSteps);
  record->elapsedHours = swap32(record->elapsedHours);
  record->rtcSeconds = swap32(record->rtcSeconds);
  record->days = swap16(record->days);
  record->watts = swap16(record->watts);
  record->pokemonMinutes = swap16(record->pokemonMinutes);
}

void pw_peer_swap(PeerInfo *record)
{
  record->dailySteps = swap32(record->dailySteps);
  record->hourSteps = swap16(record->hourSteps);

}

void pw_mirror_swap(u16 primary_address, void *record, u16 length)
{
  switch (primary_address) {
  case EEPROM_STATUS_PRIMARY: {
    if (length != sizeof(DeviceStatus)) {
      pw_host_fault("Status mirror has an invalid record size");
      return;
    }
    DeviceStatus status;
    memcpy(&status, record, sizeof(status));
    pw_status_swap(&status);
    memcpy(record, &status, sizeof(status));
    break;
  }
  case EEPROM_SAVE_PRIMARY: {
    if (length != sizeof(SaveData)) {
      pw_host_fault("Save mirror has an invalid record size");
      return;
    }
    SaveData save;
    memcpy(&save, record, sizeof(save));
    swap_save(&save);
    memcpy(record, &save, sizeof(save));
    break;
  }
  case EEPROM_BATTERY_PRIMARY: {
    if (length != sizeof(u16)) {
      pw_host_fault("Battery mirror has an invalid record size");
      return;
    }
    u16 value;
    memcpy(&value, record, sizeof(value));
    value = swap16(value);
    memcpy(record, &value, sizeof(value));
    break;
  }
  default:
    break;
  }
}

static bool eeprom_changed;

bool pw_eeprom_dirty(void)
{
  return eeprom_changed;
}

void pw_eeprom_mark_clean(void)
{
  eeprom_changed = false;
}

void pw_eeprom_read_raw(u16 address, void *out, u16 length)
{
  pw_host_eeprom_read(address, out, length);
}

void pw_eeprom_write_raw(u16 address, const void *bytes, u16 length)
{
  if (pw_host_eeprom_write(address, bytes, length)) {
    eeprom_changed = true;
  }
}

u8 EepromReadByte(u16 address)
{
  u8 value;
  pw_eeprom_read_raw(address, &value, sizeof(value));
  return value;
}

u8 EepromWriteByte(u16 address, u8 value)
{
  pw_eeprom_write_raw(address, &value, sizeof(value));
  return g_state.events.byte;
}

void EepromFill(u16 address, u16 count, u8 value)
{
  u8 page[EEPROM_PAGE_BYTES];
  memset(page, value, sizeof(page));
  while (count != 0) {
    u16 length = count > sizeof(page) ? sizeof(page) : count;
    pw_eeprom_write_raw(address, page, length);
    address += length;
    count -= length;
  }
}

void EepromFillPage(u16 address, u8 value)
{
  EepromFill(address, EEPROM_PAGE_BYTES, value);
}

/* DS records already match native little-endian order. Only these H8-owned
 * counters need conversion. A native read may span records or select complete
 * fields; byte-oriented protocol and artwork transfers use the raw API. */
typedef struct {
  u16 offset;
  u8 width;
} StoredCounter;

typedef struct {
  u16 address;
  u16 stride;
  u16 count;
  const StoredCounter *counters;
  size_t counter_count;
} CounterRegion;

static const StoredCounter halfword_counter[] = {{0, sizeof(u16)}};
static const StoredCounter word_counter[] = {{0, sizeof(u32)}};
static const StoredCounter diary_counters[] = {
    {offsetof(DiaryEntry, rtcSeconds), sizeof(u32)},
    {offsetof(DiaryEntry, ownHourSteps), sizeof(u16)},
    {offsetof(DiaryEntry, peerHourSteps), sizeof(u16)},
    {offsetof(DiaryEntry, ownDaySteps), sizeof(u32)},
    {offsetof(DiaryEntry, peerDaySteps), sizeof(u32)},
};
static const StoredCounter peer_counters[] = {
    {offsetof(PeerInfo, dailySteps), sizeof(u32)},
    {offsetof(PeerInfo, hourSteps), sizeof(u16)},
};
static const CounterRegion counter_regions[] = {
    {PW_EEPROM_MEMBER_ADDRESS(EEPROM_WALK, WalkData, watts),
     sizeof(u16), 1, halfword_counter, 1},
    {PW_EEPROM_MEMBER_ADDRESS(EEPROM_WALK, WalkData, dailySteps),
     sizeof(u32), STEP_HISTORY_DAYS, word_counter, 1},
    {PW_EEPROM_MEMBER_ADDRESS(EEPROM_WALK, WalkData, diary),
     sizeof(DiaryEntry), DIARY_ENTRIES, diary_counters,
     sizeof(diary_counters) / sizeof(diary_counters[0])},
    {EEPROM_PEER_INFO, sizeof(PeerInfo), 1, peer_counters,
     sizeof(peer_counters) / sizeof(peer_counters[0])},
};

typedef void (*CounterVisitor)(u16 address, u8 width, void *context);

static bool visit_counters(u16 address, u16 length, CounterVisitor visit,
                           void *context)
{
  if (length == 0) {
    return true;
  }
  u32 end = (u32)address + length;
  if (end > EEPROM_CAPACITY) {
    pw_host_fault("EEPROM access exceeds storage");
    return false;
  }
  for (size_t index = 0;
       index < sizeof(counter_regions) / sizeof(counter_regions[0]); ++index) {
    const CounterRegion *region = &counter_regions[index];
    u32 region_end = (u32)region->address + region->stride * region->count;
    if (end <= region->address || address >= region_end) {
      continue;
    }
    unsigned first = address > region->address
                         ? (address - region->address) / region->stride
                         : 0;
    for (unsigned record = first; record < region->count; ++record) {
      u32 base = region->address + record * region->stride;
      if (base >= end) {
        break;
      }
      for (size_t field = 0; field < region->counter_count; ++field) {
        u32 start = base + region->counters[field].offset;
        u8 width = region->counters[field].width;
        if (start >= end || start + width <= address) {
          continue;
        }
        if (start < address || start + width > end) {
          pw_host_fault("Typed EEPROM access splits a big-endian counter");
          return false;
        }
        if (visit) {
          visit((u16)start, width, context);
        }
      }
    }
  }
  return true;
}

typedef struct {
  u16 address;
  u8 *bytes;
} NativeRead;

static void decode_counter(u16 address, u8 width, void *context)
{
  NativeRead *read = context;
  u8 *bytes = read->bytes + address - read->address;
  for (unsigned index = 0; index < width / 2; ++index) {
    u8 byte = bytes[index];
    bytes[index] = bytes[width - index - 1];
    bytes[width - index - 1] = byte;
  }
}

void EepromRead(u16 address, void *out, u16 length)
{
  if (!visit_counters(address, length, NULL, NULL)) {
    memset(out, 0, length);
    return;
  }
  pw_eeprom_read_raw(address, out, length);
  NativeRead read = {address, out};
  visit_counters(address, length, decode_counter, &read);
}

typedef struct {
  u16 address;
  const u8 *bytes;
  u16 written;
} NativeWrite;

static void encode_counter(u16 address, u8 width, void *context)
{
  NativeWrite *write = context;
  u16 offset = address - write->address;
  if (offset > write->written) {
    pw_eeprom_write_raw(write->address + write->written,
                        write->bytes + write->written, offset - write->written);
  }
  u8 encoded[sizeof(u32)];
  for (unsigned index = 0; index < width; ++index) {
    encoded[index] = write->bytes[offset + width - index - 1];
  }
  pw_eeprom_write_raw(address, encoded, width);
  write->written = offset + width;
}

void EepromWrite(u16 address, void *bytes, u16 length)
{
  if (!visit_counters(address, length, NULL, NULL)) {
    return;
  }
  NativeWrite write = {address, bytes, 0};
  visit_counters(address, length, encode_counter, &write);
  if (write.written < length) {
    pw_eeprom_write_raw(address + write.written, write.bytes + write.written,
                        length - write.written);
  }
}
