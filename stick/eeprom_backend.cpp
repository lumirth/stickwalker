#include "eeprom_backend.h"

#include <FS.h>
#include <LittleFS.h>
#include <esp_partition.h>
#include <stdint.h>
#include <string.h>

extern "C" void StickEepromSetError(uint8_t value);
extern "C" uint8_t StickEepromEventByte(void);

namespace {

constexpr unsigned kImageBytes = 65536;
constexpr unsigned kPageBytes = 128;
constexpr uint32_t kMagic = 0x50574531;  // PWE1
constexpr const char *kSlots[2] = {"/pw-a.bin", "/pw-b.bin"};
constexpr const char *kTemp = "/pw-next.tmp";

struct Header {
  uint32_t magic;
  uint32_t generation;
  uint32_t crc;
};

uint8_t image[kImageBytes];
uint32_t generation = 0;
int active_slot = -1;
bool mounted = false;
bool dirty = false;
bool deferred = false;

uint32_t crc32(const uint8_t *bytes, size_t count) {
  uint32_t crc = 0xffffffff;
  for (size_t i = 0; i < count; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
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

bool read_slot(unsigned slot, uint32_t &version) {
  fs::File file = LittleFS.open(kSlots[slot], "r");
  if (!file || file.size() != kImageBytes + sizeof(Header)) return false;
  Header header;
  if (file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) !=
      sizeof(header))
    return false;
  if (header.magic != kMagic ||
      file.read(image, sizeof(image)) != sizeof(image) ||
      crc32(image, sizeof(image)) != header.crc)
    return false;
  version = header.generation;
  return true;
}

bool range_ok(uint16_t address, uint16_t length) {
  return uint32_t(address) + length <= kImageBytes;
}

void storage_error() { StickEepromSetError(1); }

bool write_span(uint16_t address, const void *source, uint16_t length) {
  if (!mounted || !source || !range_ok(address, length)) {
    storage_error();
    return false;
  }
  if (memcmp(image + address, source, length)) {
    memcpy(image + address, source, length);
    dirty = true;
  }
  if (!deferred && !StickEepromCommit()) {
    storage_error();
    return false;
  }
  return true;
}

}  // namespace

extern "C" int StickEepromMount(void) {
  if (mounted) return 1;
  if (!LittleFS.begin(false)) {
    if (!partition_blank() || !LittleFS.begin(true)) return 0;
  }
  uint32_t first_version = 0, second_version = 0;
  const bool first = read_slot(0, first_version);
  const bool second = read_slot(1, second_version);
  if (first && (!second || int32_t(first_version - second_version) > 0)) {
    read_slot(0, generation);
    active_slot = 0;
  } else if (second) {
    read_slot(1, generation);
    active_slot = 1;
  } else {
    // A mounted filesystem with other files must not be interpreted as an
    // erased Pokewalker. Fresh initialization is allowed only without slots.
    if (LittleFS.exists(kSlots[0]) || LittleFS.exists(kSlots[1])) return 0;
    memset(image, 0xff, sizeof(image));
    dirty = true;
  }
  mounted = true;
  return dirty ? StickEepromCommit() : 1;
}

extern "C" void StickEepromDefer(int enabled) {
  deferred = enabled != 0;
  if (!deferred && dirty && !StickEepromCommit()) storage_error();
}

extern "C" int StickEepromCommit(void) {
  if (!mounted) return 0;
  if (!dirty) return 1;
  const int next = active_slot == 0 ? 1 : 0;
  const Header header = {kMagic, generation + 1, crc32(image, sizeof(image))};
  LittleFS.remove(kTemp);
  fs::File file = LittleFS.open(kTemp, "w");
  if (!file) return 0;
  const bool written =
      file.write(reinterpret_cast<const uint8_t *>(&header),
                 sizeof(header)) == sizeof(header) &&
      file.write(image, sizeof(image)) == sizeof(image);
  file.flush();
  file.close();
  if (!written) return 0;
  // Validate the temporary image independently before replacing a slot.
  fs::File verify = LittleFS.open(kTemp, "r");
  Header observed;
  bool valid = verify &&
      verify.read(reinterpret_cast<uint8_t *>(&observed), sizeof(observed)) ==
          sizeof(observed) &&
      observed.magic == kMagic && observed.generation == header.generation &&
      observed.crc == header.crc;
  uint32_t check = 0xffffffff;
  uint8_t block[256];
  size_t remaining = kImageBytes;
  while (valid && remaining) {
    const size_t count = remaining < sizeof(block) ? remaining : sizeof(block);
    if (verify.read(block, count) != count) { valid = false; break; }
    for (size_t i = 0; i < count; ++i) {
      check ^= block[i];
      for (unsigned bit = 0; bit < 8; ++bit)
        check = (check >> 1) ^ (0xedb88320u & (0u - (check & 1u)));
    }
    remaining -= count;
  }
  verify.close();
  if (!valid || ~check != header.crc) return 0;
  LittleFS.remove(kSlots[next]);
  if (!LittleFS.rename(kTemp, kSlots[next])) return 0;
  active_slot = next;
  generation = header.generation;
  dirty = false;
  return 1;
}

extern "C" void EepromRead(uint16_t address, void *destination,
                            uint16_t length) {
  StickEepromSetError(0);
  if (!mounted || !destination || !range_ok(address, length)) {
    storage_error();
    if (destination) memset(destination, 0xff, length);
    return;
  }
  memcpy(destination, image + address, length);
}

extern "C" void EepromWrite(uint16_t address, void *source, uint16_t length) {
  StickEepromSetError(0);
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
  StickEepromSetError(0);
  if (!mounted || !range_ok(address, count)) {
    storage_error();
    return;
  }
  bool changed = false;
  for (unsigned i = 0; i < count; ++i) {
    changed |= image[address + i] != value;
    image[address + i] = value;
  }
  dirty |= changed;
  if (!deferred && !StickEepromCommit()) storage_error();
}

extern "C" void EepromWritePage(uint16_t address, uint8_t *source) {
  EepromWrite(address, source, kPageBytes);
}
