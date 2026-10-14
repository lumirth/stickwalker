#include "eeprom_backend.h"
#include "device_identity.h"

#include <FS.h>
#include <LittleFS.h>
#include <esp_partition.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" void StickEepromSetError(uint8_t value);
extern "C" uint8_t StickEepromEventByte(void);

namespace {

constexpr unsigned kImageBytes = 65536;
constexpr unsigned kPageBytes = 128;
constexpr uint32_t kMagic = 0x50574532;
constexpr uint32_t kJournalMagic = 0x50574a32;
constexpr uint32_t kLegacyMagic = 0x50574531;
constexpr uint32_t kLegacyJournalMagic = 0x50574a31;
constexpr const char *kMountPath = "/littlefs";
constexpr const char *kSlots[2] = {"/pw2-a.bin", "/pw2-b.bin"};
constexpr const char *kTemp = "/pw2-next.tmp";
constexpr const char *kJournal = "/pw2-journal.bin";
constexpr const char *kLegacySlots[2] = {"/pw-a.bin", "/pw-b.bin"};
constexpr const char *kLegacyJournal = "/pw-journal.bin";
constexpr size_t kJournalRecordMax = 4096;
constexpr size_t kJournalLimit = 262144;
constexpr int64_t kServiceDeadlineUs = 97700;

struct Header {
  uint32_t magic;
  uint32_t generation;
  uint32_t sequence;
  uint32_t length;
  uint32_t crc;
  uint32_t header_crc;
};

struct LegacyHeader {
  uint32_t magic;
  uint32_t generation;
  uint32_t crc;
};

struct LegacyJournalHeader {
  uint32_t magic;
  uint32_t length;
  uint32_t crc;
};

static_assert(sizeof(Header) == 24, "EEPROM header layout");
static_assert(sizeof(LegacyHeader) == 12, "Legacy EEPROM header layout");

uint8_t image[kImageBytes];
uint32_t generation = 0;
uint32_t sequence = 0;
int active_slot = -1;
bool mounted = false;
bool dirty = false;
bool deferred = false;
bool session = false;
bool failed = false;
bool timing_failed = false;
unsigned batch_depth = 0;
uint8_t journal_record[kJournalRecordMax];
size_t journal_record_length = 0;
size_t journal_size = 0;
bool journal_overflow = false;
uint32_t append_count = 0;
int64_t max_append_us = 0;
uint32_t deadline_misses = 0;

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

int open_record(const char *path, int flags) {
  char full_path[64];
  const int length = snprintf(full_path, sizeof(full_path), "%s%s", kMountPath, path);
  if (length < 0 || size_t(length) >= sizeof(full_path)) return -1;
  return open(full_path, flags, 0600);
}

bool write_record(const char *path, const Header &header, const uint8_t *bytes,
                  bool append, size_t expected_size) {
  const int fd = open_record(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
  if (fd < 0) return false;
  struct stat status;
  const bool written = fstat(fd, &status) == 0 &&
      status.st_size >= 0 && size_t(status.st_size) == expected_size &&
      write(fd, &header, sizeof(header)) == ssize_t(sizeof(header)) &&
      write(fd, bytes, header.length) == ssize_t(header.length);
  const bool synced = fsync(fd) == 0;
  const bool closed = close(fd) == 0;
  return written && synced && closed;
}

bool verify_append(const Header &header) {
  // Buffered fseek/fread can reread a 4 KiB journal prefix to reach a tiny
  // appended record. Read exactly that record through a fresh descriptor.
  const int fd = open_record(kJournal, O_RDONLY);
  if (fd < 0) return false;
  struct stat status;
  Header observed;
  bool valid = fstat(fd, &status) == 0 && status.st_size >= 0 &&
      size_t(status.st_size) == journal_size + sizeof(header) + header.length &&
      lseek(fd, off_t(journal_size), SEEK_SET) == off_t(journal_size) &&
      read(fd, &observed, sizeof(observed)) == ssize_t(sizeof(observed)) &&
      !memcmp(&observed, &header, sizeof(header));
  uint8_t block[256];
  size_t offset = 0;
  while (valid && offset < header.length) {
    const size_t count = header.length - offset < sizeof(block)
                             ? header.length - offset : sizeof(block);
    valid = read(fd, block, count) == ssize_t(count) &&
            !memcmp(block, journal_record + offset, count);
    offset += count;
  }
  const bool closed = close(fd) == 0;
  return valid && closed;
}

bool partition_blank() {
  const esp_partition_t *part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  if (!part) return false;
  uint8_t block[256];
  for (size_t offset = 0; offset < part->size; offset += sizeof(block)) {
    const size_t count =
        part->size - offset < sizeof(block) ? part->size - offset : sizeof(block);
    if (esp_partition_read(part, offset, block, count) != ESP_OK) return false;
    for (size_t i = 0; i < count; ++i)
      if (block[i] != 0xff) return false;
  }
  return true;
}

bool read_payload(fs::File &file, uint32_t expected_crc, bool load) {
  uint8_t block[256];
  uint32_t crc = 0xffffffff;
  for (size_t offset = 0; offset < sizeof(image); offset += sizeof(block)) {
    if (file.read(block, sizeof(block)) != sizeof(block)) return false;
    crc = crc_update(crc, block, sizeof(block));
    if (load) memcpy(image + offset, block, sizeof(block));
  }
  return ~crc == expected_crc;
}

bool read_checkpoint(const char *path, Header &header, bool load) {
  fs::File file = LittleFS.open(path, "r");
  return file && file.size() == sizeof(header) + sizeof(image) &&
         file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) ==
             sizeof(header) &&
         valid_header(header, kMagic) && header.length == sizeof(image) &&
         read_payload(file, header.crc, load);
}

bool read_legacy(unsigned slot, LegacyHeader &header, bool load) {
  fs::File file = LittleFS.open(kLegacySlots[slot], "r");
  return file && file.size() == sizeof(header) + sizeof(image) &&
         file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) ==
             sizeof(header) &&
         header.magic == kLegacyMagic && read_payload(file, header.crc, load);
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

bool replay_journal(bool legacy) {
  const char *path = legacy ? kLegacyJournal : kJournal;
  if (!LittleFS.exists(path)) return true;
  fs::File file = LittleFS.open(path, "r");
  if (!file) return false;
  const size_t size = file.size();
  if (!legacy) journal_size = size;
  uint32_t previous = 0;
  uint32_t journal_generation = 0;
  while (file.position() < size) {
    Header header = {};
    if (legacy) {
      LegacyJournalHeader old;
      if (size - file.position() < sizeof(old)) break;
      if (file.read(reinterpret_cast<uint8_t *>(&old), sizeof(old)) !=
          sizeof(old) || old.magic != kLegacyJournalMagic) return false;
      header.length = old.length;
      header.crc = old.crc;
    } else {
      if (size - file.position() < sizeof(header)) break;
      if (file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) !=
          sizeof(header) || !valid_header(header, kJournalMagic)) return false;
      if (previous && (previous == UINT32_MAX ||
                       header.sequence != previous + 1 ||
                       header.generation != journal_generation)) return false;
      previous = header.sequence;
      journal_generation = header.generation;
      if (header.generation > generation) return false;
    }
    if (!header.length || header.length > sizeof(journal_record)) return false;
    if (size - file.position() < header.length) break;
    if (file.read(journal_record, header.length) != header.length ||
        crc32(journal_record, header.length) != header.crc) return false;
    if (!legacy && header.sequence <= sequence) {
      if (header.generation >= generation) return false;
      continue;
    }
    if (!legacy && (header.generation != generation || sequence == UINT32_MAX ||
                    header.sequence != sequence + 1)) return false;
    if (!apply_patches(header.length)) return false;
    if (!legacy) sequence = header.sequence;
  }
  dirty = true;
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
  if (journal_overflow || journal_size + sizeof(Header) + journal_record_length >
                              kJournalLimit)
    return !session && StickEepromCommit();
  if (!journal_record_length) return true;
  const Header header = make_header(kJournalMagic, generation, sequence + 1,
                                    journal_record, journal_record_length);
  if (!write_record(kJournal, header, journal_record, true, journal_size))
    return false;
  if (!verify_append(header)) return false;
  sequence = header.sequence;
  journal_size += sizeof(header) + header.length;
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
    if (elapsed >= kServiceDeadlineUs) {
      ++deadline_misses;
      timing_failed = true;
      StickEepromSetError(1);
    }
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
  const bool a = read_checkpoint(kSlots[0], first, false);
  const bool b = read_checkpoint(kSlots[1], second, false);
  if (a || b) {
    if (a && b &&
        ((first.generation == second.generation &&
          memcmp(&first, &second, sizeof(first))) ||
         (first.generation > second.generation && first.sequence <= second.sequence) ||
         (second.generation > first.generation && second.sequence <= first.sequence)))
      return false;
    active_slot = a && (!b || first.generation > second.generation) ? 0 : 1;
    Header selected;
    if (!read_checkpoint(kSlots[active_slot], selected, true)) return false;
    generation = selected.generation;
    sequence = selected.sequence;
    return replay_journal(false);
  }
  if (LittleFS.exists(kSlots[0]) || LittleFS.exists(kSlots[1]) ||
      LittleFS.exists(kJournal)) return false;
  LegacyHeader old_a = {}, old_b = {};
  const bool legacy_a = read_legacy(0, old_a, false);
  const bool legacy_b = read_legacy(1, old_b, false);
  if (legacy_a || legacy_b) {
    if (legacy_a && legacy_b && old_a.generation == old_b.generation &&
        old_a.crc != old_b.crc) return false;
    const unsigned slot = legacy_a &&
        (!legacy_b || int32_t(old_a.generation - old_b.generation) > 0) ? 0 : 1;
    LegacyHeader selected;
    if (!read_legacy(slot, selected, true) || !replay_journal(true)) return false;
  } else {
    if (LittleFS.exists(kLegacySlots[0]) || LittleFS.exists(kLegacySlots[1]) ||
        LittleFS.exists(kLegacyJournal) || LittleFS.exists("/pw-next.tmp") ||
        LittleFS.exists(kTemp)) return false;
    memset(image, 0xff, sizeof(image));
  }
  dirty = true;
  return true;
}

}

extern "C" int StickEepromMount(void) {
  if (mounted) return !failed;
  if (failed) return 0;
  if (!LittleFS.begin(false, kMountPath)) {
    if (!partition_blank() || !LittleFS.begin(true, kMountPath)) return 0;
  }
  if (!load_image()) { storage_error(); return 0; }
  uint8_t factory_mac[6];
  if (esp_efuse_mac_get_default(factory_mac) != ESP_OK) return 0;
  dirty |= StickIdentityRepair(image, factory_mac) != 0;
  mounted = true;
  if (!StickEepromCommit()) { storage_error(); mounted = false; return 0; }
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
  const size_t total = LittleFS.totalBytes();
  const size_t used = LittleFS.usedBytes();
  if (total < used || total - used < kJournalLimit + kImageBytes) {
    storage_error();
    return 0;
  }
  session = true;
  timing_failed = false;
  append_count = 0;
  max_append_us = 0;
  deadline_misses = 0;
  return 1;
}

extern "C" int StickEepromSessionEnd(void) {
  session = false;
  esp_log_write(failed || timing_failed ? ESP_LOG_ERROR : ESP_LOG_INFO,
                "pw-eeprom",
                "PW_EEPROM_SESSION appends=%lu max_us=%lld deadline_misses=%lu failed=%d\n",
                static_cast<unsigned long>(append_count),
                static_cast<long long>(max_append_us),
                static_cast<unsigned long>(deadline_misses), int(failed));
  return !failed && !timing_failed;
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
  const Header header = make_header(kMagic, generation + 1, sequence + 1,
                                    image, sizeof(image));
  if (!write_record(kTemp, header, image, false, 0)) return 0;
  Header observed;
  if (!read_checkpoint(kTemp, observed, false) ||
      memcmp(&observed, &header, sizeof(header))) return 0;
  if (LittleFS.exists(kSlots[next]) && !LittleFS.remove(kSlots[next])) return 0;
  if (!LittleFS.rename(kTemp, kSlots[next])) return 0;
  if (!read_checkpoint(kSlots[next], observed, false) ||
      memcmp(&observed, &header, sizeof(header))) return 0;
  active_slot = next;
  generation = header.generation;
  sequence = header.sequence;
  if (LittleFS.exists(kJournal) && !LittleFS.remove(kJournal)) return 0;
  dirty = false;
  journal_size = 0;
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
  StickEepromSetError(failed || timing_failed);
  if (!mounted || !destination || !range_ok(address, length)) {
    storage_error();
    if (destination) memset(destination, 0xff, length);
    return;
  }
  memcpy(destination, image + address, length);
}

extern "C" void EepromWrite(uint16_t address, void *source, uint16_t length) {
  StickEepromSetError(failed || timing_failed);
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
  StickEepromSetError(failed || timing_failed);
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
