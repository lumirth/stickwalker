/* Run the actual mount/journal/checkpoint code with fault-injectable files. */
#include "../eeprom_backend.cpp"
#include <cassert>
#include <cstdio>

extern "C" void StickEepromSetError(uint8_t)
{
}
extern "C" uint8_t StickEepromEventByte(void)
{
  return 0;
}

static void reboot()
{
  mounted = dirty = deferred = journal_overflow = journal_poisoned = false;
  generation = batch_depth = 0;
  active_slot = -1;
  journal_record_length = journal_size = 0;
  memset(image, 0, sizeof(image));
  fs::writeBudget = -1;
  fs::failRename = false;
}

static void seed(const std::vector<uint8_t> &save)
{
  reboot();
  fs::files.clear();
  Header header = {kMagic, 1, crc32(save.data(), save.size())};
  auto &file = fs::files[kSlots[0]];
  file.resize(sizeof(header) + save.size());
  memcpy(file.data(), &header, sizeof(header));
  memcpy(file.data() + sizeof(header), save.data(), save.size());
}

static void verify_repair(const std::vector<uint8_t> &before)
{
  // The EEPROM format stores two 40-byte IDs, each followed by a checksum.
  constexpr unsigned primary = 0x0083, backup = 0x0183, id_bytes = 40;
  const uint8_t *id = image + primary;
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  assert(memcmp(id + id_bytes - sizeof(mac), mac, sizeof(mac)) == 0);
  assert(memcmp(id, image + backup, id_bytes + 1) == 0);
  unsigned sum = 1;
  for (unsigned i = 0; i < id_bytes; ++i)
    sum += id[i];
  assert(id[id_bytes] == uint8_t(sum));
  for (unsigned i = 0; i < before.size(); ++i) {
    const bool master =
        (i >= primary && i <= primary + id_bytes) ||
        (i >= backup && i <= backup + id_bytes);
    if (!master)
      assert(image[i] == before[i]);
  }
}

int main()
{
  std::vector<uint8_t> before(kImageBytes, 0xff);
  seed(before);
  assert(StickEepromMount());
  verify_repair(before);
  const auto checkpoint = fs::files;
  reboot();
  assert(StickEepromMount());
  verify_repair(before);
  assert(fs::files == checkpoint); // No extra writes on a repaired boot.

  // Fail at the header, mid-image, or rename. Every retry retains the old save
  // and completes the same identity repair after a simulated power cycle.
  for (long budget : {0L, 6L, 1000L, 65547L, -1L}) {
    seed(before);
    const auto original = fs::files.at(kSlots[0]);
    fs::writeBudget = budget;
    fs::failRename = budget == -1;
    assert(!StickEepromMount());
    assert(fs::files.at(kSlots[0]) == original);
    reboot();
    assert(StickEepromMount());
    verify_repair(before);
  }

  // The migration must operate on replayed game progress, not the older slot.
  seed(before);
  const uint8_t patch[] = {0x90, 0xce, 1, 0, 0x42};
  const JournalHeader journal = {kJournalMagic, sizeof(patch),
                                 crc32(patch, sizeof(patch))};
  auto &log = fs::files[kJournal];
  log.resize(sizeof(journal) + sizeof(patch));
  memcpy(log.data(), &journal, sizeof(journal));
  memcpy(log.data() + sizeof(journal), patch, sizeof(patch));
  before[0xce90] = 0x42;
  assert(StickEepromMount());
  verify_repair(before);
  reboot();
  assert(StickEepromMount());
  verify_repair(before);
  puts("automatic identity checkpoint, interrupted writes and journal replay: "
       "PASS");
}
