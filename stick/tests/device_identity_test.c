#include "device_identity.h"
#include "eeprom_map.h"
#include "records.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static u8 image[EEPROM_CAPACITY], before[EEPROM_CAPACITY];
static const u8 mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};

static void seal(unsigned address, unsigned length)
{
  u8 sum = 1;
  for (unsigned i = 0; i < length; ++i)
    sum += image[address + i];
  image[address + length] = sum;
}

static int identity_byte(unsigned address)
{
  return (address >= EEPROM_ID_PRIMARY &&
          address <= EEPROM_ID_PRIMARY + DEVICE_ID_BYTES) ||
         (address >= EEPROM_ID_BACKUP &&
          address <= EEPROM_ID_BACKUP + DEVICE_ID_BYTES) ||
         (address >= EEPROM_STATUS_PRIMARY + 16 &&
          address < EEPROM_STATUS_PRIMARY + 56) ||
         (address >= EEPROM_STATUS_BACKUP + 16 &&
          address < EEPROM_STATUS_BACKUP + 56) ||
         address == EEPROM_STATUS_PRIMARY + sizeof(DeviceStatus) ||
         address == EEPROM_STATUS_BACKUP + sizeof(DeviceStatus) ||
         (address >= EEPROM_OWN_RECORDS + 8 &&
          address < EEPROM_OWN_RECORDS + 48);
}

static void verify_repair(void)
{
  u8 id[DEVICE_ID_BYTES];
  memcpy(before, image, sizeof(image));
  assert(StickIdentityRepair(image, mac));
  for (unsigned i = 0; i < sizeof(image); ++i)
    if (!identity_byte(i))
      assert(before[i] == image[i]);
  memcpy(id, image + EEPROM_ID_PRIMARY, sizeof(id));
  assert(memcmp(id, image + EEPROM_ID_BACKUP, sizeof(id)) == 0);
  assert(memcmp(id, image + EEPROM_STATUS_PRIMARY + 16, sizeof(id)) == 0);
  assert(memcmp(id, image + EEPROM_STATUS_BACKUP + 16, sizeof(id)) == 0);
  assert(memcmp(id, image + EEPROM_OWN_RECORDS + 8, sizeof(id)) == 0);
  memcpy(before, image, sizeof(image));
  assert(!StickIdentityRepair(image, mac));
  assert(memcmp(before, image, sizeof(image)) == 0);
}

int main(int argc, char **argv)
{
  if (argc == 2) {
    /* Read a retained save and test migration on the in-memory copy. */
    FILE *file = fopen(argv[1], "rb");
    assert(file && fread(image, 1, sizeof(image), file) == sizeof(image));
    assert(fgetc(file) == EOF);
    fclose(file);
    verify_repair();
    puts("retained save: identity repaired; every other byte preserved");
    return 0;
  }

  for (unsigned empty = 0; empty <= 255; empty += 255) {
    /* Distinct nonzero payloads exercise every protected byte, including the
     * staged record, which overlaps active walk data and must stay untouched.
     */
    for (unsigned i = 0; i < sizeof(image); ++i)
      image[i] = (u8)(i * 37 + 19);
    memset(image + EEPROM_ID_PRIMARY, empty, DEVICE_ID_BYTES);
    memset(image + EEPROM_ID_BACKUP, empty, DEVICE_ID_BYTES);
    memset(image + EEPROM_STATUS_PRIMARY + 16, empty, DEVICE_ID_BYTES);
    memset(image + EEPROM_STATUS_BACKUP + 16, empty, DEVICE_ID_BYTES);
    image[EEPROM_STATUS_PRIMARY + STATUS_FLAGS_OFFSET] |= 3;
    image[EEPROM_STATUS_BACKUP + STATUS_FLAGS_OFFSET] |= 3;
    seal(EEPROM_ID_PRIMARY, DEVICE_ID_BYTES);
    seal(EEPROM_ID_BACKUP, DEVICE_ID_BYTES);
    seal(EEPROM_STATUS_PRIMARY, sizeof(DeviceStatus));
    seal(EEPROM_STATUS_BACKUP, sizeof(DeviceStatus));
    verify_repair();
  }

  u8 original[DEVICE_ID_BYTES];
  memcpy(original, image + EEPROM_ID_PRIMARY, sizeof(original));
  /* Existing identities take precedence over the board-derived default. */
  const u8 otherMac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x56};
  assert(!StickIdentityRepair(image, otherMac));
  image[EEPROM_ID_PRIMARY + DEVICE_ID_BYTES] ^= 1;
  assert(StickIdentityRepair(image, otherMac));
  assert(memcmp(original, image + EEPROM_ID_PRIMARY, sizeof(original)) == 0);
  memset(image + EEPROM_ID_PRIMARY, 255, DEVICE_ID_BYTES + 1);
  memset(image + EEPROM_ID_BACKUP, 255, DEVICE_ID_BYTES + 1);
  assert(StickIdentityRepair(image, otherMac));
  assert(memcmp(original, image + EEPROM_ID_PRIMARY, sizeof(original)) == 0);

  /* Corrupt status stays available for normal mirrored-read recovery. */
  image[EEPROM_STATUS_PRIMARY + sizeof(DeviceStatus)] ^= 1;
  memcpy(before, image, sizeof(image));
  assert(!StickIdentityRepair(image, mac));
  assert(memcmp(before, image, sizeof(image)) == 0);

  memset(image, 255, sizeof(image));
  assert(StickIdentityRepair(image, otherMac));
  assert(memcmp(original, image + EEPROM_ID_PRIMARY, sizeof(original)) != 0);
  memset(image, 255, sizeof(image));
  assert(StickIdentityRepair(image, mac));
  assert(memcmp(original, image + EEPROM_ID_PRIMARY, sizeof(original)) == 0);
  /* Individual zero and FF bytes are valid inside a provisioned identity. */
  memset(image + EEPROM_ID_PRIMARY, 0x42, DEVICE_ID_BYTES);
  image[EEPROM_ID_PRIMARY] = 0xff;
  image[EEPROM_ID_PRIMARY + 1] = 0;
  seal(EEPROM_ID_PRIMARY, DEVICE_ID_BYTES);
  memcpy(original, image + EEPROM_ID_PRIMARY, sizeof(original));
  assert(StickIdentityRepair(image, otherMac));
  assert(memcmp(original, image + EEPROM_ID_PRIMARY, sizeof(original)) == 0);
  assert(memcmp(original, image + EEPROM_ID_BACKUP, sizeof(original)) == 0);
  puts("identity provisioning, preservation, mirror recovery and idempotence: "
       "PASS");
  return 0;
}
