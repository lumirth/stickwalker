#include "eeprom_backend.h"
#include "device_identity.h"

#include <esp_partition.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern "C" void StickEepromSetError(uint8_t value);
extern "C" uint8_t StickEepromEventByte(void);

namespace {

constexpr unsigned kImageBytes = 65536;
constexpr unsigned kPageBytes = 128;
constexpr size_t kSectorBytes = 4096;
constexpr size_t kCheckpointSpan = 0x11000;
constexpr size_t kJournalOffset = 2 * kCheckpointSpan;
constexpr size_t kJournalRecordMax = 4096;
constexpr size_t kJournalLimit = 262144;
constexpr size_t kRecordAlignment = 256;
constexpr uint32_t kMagic = 0x53574531;
constexpr uint32_t kJournalMagic = 0x53574a31;
constexpr uint32_t kCommitted = 0x54494d43;

struct Header {
  uint32_t magic;
  uint32_t generation;
  uint32_t sequence;
  uint32_t length;
  uint32_t crc;
  uint32_t header_crc;
};
static_assert(sizeof(Header) == 24, "EEPROM header layout");
constexpr size_t kPayloadOffset = sizeof(Header) + sizeof(kCommitted);
static_assert(kPayloadOffset + kImageBytes <= kCheckpointSpan,
               "Checkpoint fits its flash slot");

const esp_partition_t *store = nullptr;
uint8_t image[kImageBytes];
uint32_t generation = 0;
uint32_t sequence = 0;
int active_slot = -1;
bool mounted = false;
bool dirty = false;
bool deferred = false;
bool session = false;
bool failed = false;
unsigned batch_depth = 0;
uint8_t journal_record[kJournalRecordMax];
size_t journal_record_length = 0;
size_t journal_size = 0;
bool journal_overflow = false;
uint32_t append_count = 0;
int64_t max_append_us = 0;

uint32_t crc_update(uint32_t crc, const uint8_t *bytes, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return crc;
}
uint32_t crc32(const void *bytes, size_t count) {
  return ~crc_update(0xffffffff, static_cast<const uint8_t *>(bytes), count);
}
bool valid_header(const Header &header, uint32_t magic) {
  return header.magic == magic && header.generation && header.sequence &&
         header.header_crc == crc32(&header, offsetof(Header, header_crc));
}
Header make_header(uint32_t magic, uint32_t version, uint32_t serial,
                   const uint8_t *bytes, size_t length) {
  Header header = {magic, version, serial, uint32_t(length),
                   crc32(bytes, length), 0};
  header.header_crc = crc32(&header, offsetof(Header, header_crc));
  return header;
}
size_t record_span(size_t length) {
  return (kPayloadOffset + length + kRecordAlignment - 1) &
         ~(kRecordAlignment - 1);
}
bool flash_read(size_t offset, void *bytes, size_t length) {
  return esp_partition_read(store, offset, bytes, length) == ESP_OK;
}
bool region_blank(size_t offset, size_t length) {
  uint8_t block[256];
  while (length) {
    const size_t count = length < sizeof(block) ? length : sizeof(block);
    if (!flash_read(offset, block, count)) return false;
    for (size_t i = 0; i < count; ++i) if (block[i] != 0xff) return false;
    offset += count;
    length -= count;
  }
  return true;
}
bool clear_region(size_t offset, size_t length) {
  if (session || offset % kSectorBytes || length % kSectorBytes) return false;
  for (size_t end = offset + length; offset < end; offset += kSectorBytes) {
    if (!region_blank(offset, kSectorBytes) &&
        esp_partition_erase_range(store, offset, kSectorBytes) != ESP_OK)
      return false;
    if (!region_blank(offset, kSectorBytes)) return false;
  }
  return true;
}
bool clear_journal() {
  if (!clear_region(kJournalOffset, kJournalLimit)) return false;
  journal_size = 0;
  return true;
}
bool write_committed(size_t offset, const Header &header, const uint8_t *bytes) {
  // The SDK waits for NOR programming to complete. Header and body are checked
  // independently before the final publication word; replies never erase flash.
  if (esp_partition_write(store, offset + kPayloadOffset, bytes,
                           header.length) != ESP_OK ||
      esp_partition_write(store, offset, &header, sizeof(header)) != ESP_OK)
    return false;
  Header observed;
  if (!flash_read(offset, &observed, sizeof(observed)) ||
      memcmp(&observed, &header, sizeof(header))) return false;
  uint8_t block[256];
  for (size_t position = 0; position < header.length;) {
    const size_t count = header.length - position < sizeof(block) ?
                         header.length - position : sizeof(block);
    if (!flash_read(offset + kPayloadOffset + position, block, count) ||
        memcmp(block, bytes + position, count)) return false;
    position += count;
  }
  uint32_t marker = kCommitted;
  return esp_partition_write(store, offset + sizeof(Header), &marker,
                               sizeof(marker)) == ESP_OK &&
         flash_read(offset + sizeof(Header), &marker, sizeof(marker)) &&
         marker == kCommitted;
}
bool read_payload(size_t offset, uint32_t expected_crc, bool load) {
  uint8_t block[256];
  uint32_t crc = 0xffffffff;
  for (size_t position = 0; position < sizeof(image); position += sizeof(block)) {
    if (!flash_read(offset + kPayloadOffset + position, block, sizeof(block)))
      return false;
    crc = crc_update(crc, block, sizeof(block));
    if (load) memcpy(image + position, block, sizeof(block));
  }
  return ~crc == expected_crc;
}
bool read_checkpoint(unsigned slot, Header &header, bool load) {
  const size_t offset = size_t(slot) * kCheckpointSpan;
  uint32_t marker;
  return flash_read(offset, &header, sizeof(header)) &&
         flash_read(offset + sizeof(Header), &marker, sizeof(marker)) &&
         marker == kCommitted && valid_header(header, kMagic) &&
         header.length == sizeof(image) && read_payload(offset, header.crc, load);
}
bool range_ok(uint16_t address, uint16_t length) {
  return uint32_t(address) + length <= kImageBytes;
}
void storage_error() {
  if (!failed)
    esp_log_write(ESP_LOG_ERROR, "pw-eeprom", "PW_EEPROM_FAILURE restart_required=1\n");
  failed = true;
  StickEepromSetError(1);
}
bool apply_patches(size_t length) {
  size_t offset = 0;
  while (offset < length) {
    if (length - offset < 4) return false;
    const unsigned address = unsigned(journal_record[offset]) |
                             (unsigned(journal_record[offset + 1]) << 8);
    const unsigned count = unsigned(journal_record[offset + 2]) |
                           (unsigned(journal_record[offset + 3]) << 8);
    offset += 4;
    if (address + count > kImageBytes || length - offset < count) return false;
    memcpy(image + address, journal_record + offset, count);
    offset += count;
  }
  return true;
}
bool replay_journal() {
  uint32_t previous = 0;
  uint32_t journal_generation = 0;
  while (journal_size < kJournalLimit) {
    const size_t offset = kJournalOffset + journal_size;
    Header header;
    uint32_t marker;
    if (!flash_read(offset, &header, sizeof(header)) ||
        !flash_read(offset + sizeof(Header), &marker, sizeof(marker))) return false;
    if (marker != kCommitted) break;
    if (!valid_header(header, kJournalMagic) || !header.length ||
        header.length > sizeof(journal_record) ||
        record_span(header.length) > kJournalLimit - journal_size) return false;
    if (previous && (previous == UINT32_MAX || header.sequence != previous + 1 ||
                     header.generation != journal_generation)) return false;
    previous = header.sequence;
    journal_generation = header.generation;
    if (header.generation > generation ||
        !flash_read(offset + kPayloadOffset, journal_record, header.length) ||
        crc32(journal_record, header.length) != header.crc) return false;
    journal_size += record_span(header.length);
    if (header.sequence <= sequence) {
      if (header.generation >= generation) return false;
      continue;
    }
    if (header.generation != generation || sequence == UINT32_MAX ||
        header.sequence != sequence + 1 || !apply_patches(header.length)) return false;
    sequence = header.sequence;
    dirty = true;
  }
  return true;
}

bool append_patch(uint16_t address, uint16_t length) {
  if (journal_record_length + 4u + length > sizeof(journal_record)) {
    journal_overflow = true;
    return false;
  }
  uint8_t *next = journal_record + journal_record_length;
  next[0] = uint8_t(address);
  next[1] = uint8_t(address >> 8);
  next[2] = uint8_t(length);
  next[3] = uint8_t(length >> 8);
  memcpy(next + 4, image + address, length);
  journal_record_length += 4u + length;
  return true;
}

bool append_journal_record() {
  if (failed || sequence == UINT32_MAX) return false;
  if (journal_overflow || journal_size + record_span(journal_record_length) >
                              kJournalLimit)
    return !session && StickEepromCommit();
  if (!journal_record_length) return true;
  const Header header = make_header(kJournalMagic, generation, sequence + 1,
                                    journal_record, journal_record_length);
  const size_t span = record_span(header.length);
  if (journal_size + span > kJournalLimit) return !session && StickEepromCommit();
  const size_t offset = kJournalOffset + journal_size;
  if (!region_blank(offset, span) ||
      !write_committed(offset, header, journal_record)) return false;
  sequence = header.sequence;
  journal_size += span;
  journal_record_length = 0;
  return true;
}

bool append_journal() {
  if (!journal_record_length && !journal_overflow) return !failed;
  const int64_t start = esp_timer_get_time();
  const bool result = append_journal_record();
  if (session) {
    const int64_t elapsed = esp_timer_get_time() - start;
    ++append_count;
    if (elapsed > max_append_us) max_append_us = elapsed;
  }
  return result;
}

bool write_span(uint16_t address, const void *source, uint16_t length) {
  if (!mounted || failed || !source || !range_ok(address, length)) {
    storage_error();
    return false;
  }
  if (!memcmp(image + address, source, length)) return true;
  memcpy(image + address, source, length);
  dirty = true;
  if (!deferred) {
    if (!batch_depth) {
      journal_record_length = 0;
      journal_overflow = false;
    }
    append_patch(address, length);
    if (!batch_depth && !append_journal()) {
      storage_error();
      return false;
    }
  }
  return true;
}

bool load_image() {
  Header first = {}, second = {};
  const bool a = read_checkpoint(0, first, false);
  const bool b = read_checkpoint(1, second, false);
  uint32_t first_marker, second_marker;
  if (!flash_read(sizeof(Header), &first_marker, sizeof(first_marker)) ||
      !flash_read(kCheckpointSpan + sizeof(Header), &second_marker,
                   sizeof(second_marker)) ||
      (first_marker == kCommitted && !a) ||
      (second_marker == kCommitted && !b)) return false;
  if (a || b) {
    if (a && b &&
        ((first.generation == second.generation &&
          memcmp(&first, &second, sizeof(first))) ||
         (first.generation > second.generation && first.sequence <= second.sequence) ||
         (second.generation > first.generation && second.sequence <= first.sequence)))
      return false;
    active_slot = a && (!b || first.generation > second.generation) ? 0 : 1;
    Header selected;
    if (!read_checkpoint(active_slot, selected, true)) return false;
    generation = selected.generation;
    sequence = selected.sequence;
    return replay_journal();
  }
  // An unsupported or damaged store needs explicit initialization. Only blank
  // flash creates a new walker; no save readers or conversions are retained.
  if (!region_blank(0, store->size)) return false;
  memset(image, 0xff, sizeof(image));
  dirty = true;
  return true;
}

}

extern "C" int StickEepromMount(void) {
  if (mounted) return !failed;
  if (failed) return 0;
  store = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                   ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  if (!store || store->size < kJournalOffset + kJournalLimit) return 0;
  if (!load_image()) { storage_error(); return 0; }
  uint8_t factory_mac[6];
  if (esp_efuse_mac_get_default(factory_mac) != ESP_OK) return 0;
  dirty |= StickIdentityRepair(image, factory_mac) != 0;
  mounted = true;
  const bool existing_checkpoint = !dirty;
  if (!StickEepromCommit() || (existing_checkpoint && !clear_journal())) {
    storage_error(); mounted = false; return 0;
  }
  return 1;
}

#ifdef PW_STICK_BENCH_CONTROL
extern "C" const unsigned char *StickEepromSnapshot(void) {
  return mounted && !deferred && !session && !failed ? image : nullptr;
}

extern "C" int StickEepromBenchErase(void) {
  if (!mounted || deferred || session || failed) return 0;
  memset(image, 0xff, sizeof(image));
  dirty = true;
  return StickEepromCommit();
}
#endif

extern "C" void StickEepromDefer(int enabled) {
  if (session || batch_depth) { storage_error(); return; }
  deferred = enabled != 0;
  if (!deferred && !StickEepromCommit()) storage_error();
}

extern "C" int StickEepromSessionBegin(void) {
  if (!mounted || failed || session || deferred || batch_depth ||
      !StickEepromCommit()) {
    storage_error();
    return 0;
  }
  session = true;
  append_count = 0;
  max_append_us = 0;
  return 1;
}

extern "C" int StickEepromSessionEnd(void) {
  session = false;
  esp_log_write(failed ? ESP_LOG_ERROR : ESP_LOG_INFO,
                "pw-eeprom",
                "PW_EEPROM_SESSION appends=%lu max_us=%lld failed=%d\n",
                static_cast<unsigned long>(append_count),
                static_cast<long long>(max_append_us), int(failed));
  return !failed;
}

extern "C" int StickEepromHealthy(void) {
  return mounted && !failed;
}

extern "C" void StickEepromBatchBegin(void) {
  if (!batch_depth) { journal_record_length = 0; journal_overflow = false; }
  ++batch_depth;
}

extern "C" int StickEepromBatchEnd(void) {
  if (!batch_depth) { storage_error(); return 0; }
  --batch_depth;
  if (failed || (!batch_depth && !deferred && !append_journal())) {
    storage_error();
    return 0;
  }
  return 1;
}

static int checkpoint_image() {
  if (!mounted || failed || session) return 0;
  if (!dirty) return 1;
  if (generation == UINT32_MAX || sequence == UINT32_MAX) return 0;
  const int next = active_slot == 0 ? 1 : 0;
  const size_t offset = size_t(next) * kCheckpointSpan;
  const Header header = make_header(kMagic, generation + 1, sequence + 1,
                                    image, sizeof(image));
  // Retire the inactive slot before erasing it. A reset during replacement
  // leaves the active checkpoint usable instead of a published torn body.
  uint32_t marker;
  if (!flash_read(offset + sizeof(Header), &marker, sizeof(marker))) return 0;
  if (marker == kCommitted) {
    marker = 0;
    if (esp_partition_write(store, offset + sizeof(Header), &marker,
                             sizeof(marker)) != ESP_OK ||
        !flash_read(offset + sizeof(Header), &marker, sizeof(marker)) || marker)
      return 0;
  }
  if (!clear_region(offset, kCheckpointSpan) ||
      !write_committed(offset, header, image)) return 0;
  Header observed;
  if (!read_checkpoint(next, observed, false) ||
      memcmp(&observed, &header, sizeof(header))) return 0;
  // Publication precedes erasure. A reset here replays/skips old records using
  // the published generation/sequence, then clears every dirty journal sector.
  active_slot = next;
  generation = header.generation;
  sequence = header.sequence;
  if (!clear_journal()) return 0;
  dirty = false;
  journal_overflow = false;
  journal_record_length = 0;
  return 1;
}

extern "C" int StickEepromCommit(void) {
  if (checkpoint_image()) return 1;
  storage_error();
  return 0;
}

extern "C" void EepromRead(uint16_t address, void *destination,
                            uint16_t length) {
  StickEepromSetError(failed);
  if (!mounted || !destination || !range_ok(address, length)) {
    storage_error();
    if (destination) memset(destination, 0xff, length);
    return;
  }
  memcpy(destination, image + address, length);
}

extern "C" void EepromWrite(uint16_t address, void *source, uint16_t length) {
  StickEepromSetError(failed);
  write_span(address, source, length);
}

extern "C" uint8_t EepromReadByte(uint16_t address) {
  uint8_t value = 0xff;
  EepromRead(address, &value, 1);
  return value;
}

extern "C" uint8_t EepromWriteByte(uint16_t address, uint8_t value) {
  EepromWrite(address, &value, 1);
  return StickEepromEventByte();
}

extern "C" void EepromFillPage(uint16_t address, uint8_t value) {
  uint8_t bytes[kPageBytes];
  memset(bytes, value, sizeof(bytes));
  EepromWrite(address, bytes, sizeof(bytes));
}

extern "C" void EepromFill(uint16_t address, uint16_t count, uint8_t value) {
  StickEepromSetError(failed);
  if (!mounted || failed || !range_ok(address, count)) {
    storage_error();
    return;
  }
  uint8_t bytes[kPageBytes];
  memset(bytes, value, sizeof(bytes));
  StickEepromBatchBegin();
  for (unsigned offset = 0; offset < count; offset += sizeof(bytes)) {
    const uint16_t length = count - offset < sizeof(bytes)
                                ? count - offset : sizeof(bytes);
    if (!write_span(address + offset, bytes, length)) break;
  }
  StickEepromBatchEnd();
}

extern "C" void EepromWritePage(uint16_t address, uint8_t *source) {
  EepromWrite(address, source, kPageBytes);
}
