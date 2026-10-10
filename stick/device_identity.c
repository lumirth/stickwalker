#include "device_identity.h"
#include "application/pw_eeprom_m95512.h"
#include "eeprom_map.h"
#include "records.h"
#include <string.h>

static u8 checksum(const u8 *bytes, unsigned count)
{
  u8 sum = 1;
  while (count--)
    sum += *bytes++;
  return sum;
}

static int valid_record(const u8 *bytes, unsigned count)
{
  return checksum(bytes, count) == bytes[count];
}

static int usable_id(const u8 *id)
{
  unsigned i;
  u8 any = 0, all = 0xff;
  for (i = 0; i < DEVICE_ID_BYTES; ++i) {
    any |= id[i];
    all &= id[i];
  }
  /* Reset and day rollover use these whole-ID values for empty history. */
  return any != 0 && all != 0xff;
}

static int replace(u8 *destination, const u8 *source, unsigned count)
{
  if (memcmp(destination, source, count) == 0)
    return 0;
  memcpy(destination, source, count);
  return 1;
}

int StickIdentityRepair(u8 *image, const u8 factoryMac[6])
{
  const unsigned masters[] = {EEPROM_ID_PRIMARY, EEPROM_ID_BACKUP};
  const unsigned statuses[] = {EEPROM_STATUS_PRIMARY, EEPROM_STATUS_BACKUP};
  u8 id[DEVICE_ID_BYTES + 1];
  unsigned i;
  int found = 0, changed = 0, registered = 0;

  /* Keep a provisioned identity, including when only its backup survives.
   * Status is a recovery source if both master copies have been lost. */
  for (i = 0; i < 2 && !found; ++i) {
    const u8 *master = image + masters[i];
    if (valid_record(master, DEVICE_ID_BYTES) && usable_id(master)) {
      memcpy(id, master, DEVICE_ID_BYTES);
      found = 1;
    }
  }
  for (i = 0; i < 2 && !found; ++i) {
    const u8 *status = image + statuses[i];
    const u8 *savedId = status + offsetof(DeviceStatus, deviceId);
    if (valid_record(status, sizeof(DeviceStatus)) && usable_id(savedId)) {
      memcpy(id, savedId, DEVICE_ID_BYTES);
      found = 1;
    }
  }
  if (!found) {
    /* Use all six bytes of the factory MAC so each chip gets a distinct
     * identity that survives a full flash erase. */
    memset(id, 0, DEVICE_ID_BYTES);
    memcpy(id, "Stickwalker v1", sizeof("Stickwalker v1") - 1);
    memcpy(id + DEVICE_ID_BYTES - 6, factoryMac, 6);
  }
  id[DEVICE_ID_BYTES] = checksum(id, DEVICE_ID_BYTES);
  for (i = 0; i < 2; ++i)
    changed |= replace(image + masters[i], id, sizeof(id));

  for (i = 0; i < 2; ++i) {
    u8 *status = image + statuses[i];
    /* The normal mirrored read repairs damaged status copies. Updating a
     * damaged copy's checksum here would make its corrupt payload appear
     * valid. */
    if (!valid_record(status, sizeof(DeviceStatus)))
      continue;
    registered |= status[STATUS_FLAGS_OFFSET] & 1;
    changed |=
        replace(status + offsetof(DeviceStatus, deviceId), id, DEVICE_ID_BYTES);
    status[sizeof(DeviceStatus)] = checksum(status, sizeof(DeviceStatus));
  }
  if (registered)
    changed |=
        replace(image + EEPROM_OWN_RECORDS + offsetof(PeerRecords, deviceId),
                id, DEVICE_ID_BYTES);
  return changed;
}

void StickIdentitySyncOwnRecord(void)
{
  u8 id[DEVICE_ID_BYTES];
  EepromMirrorRead(EEPROM_ID_PRIMARY, EEPROM_ID_BACKUP, id, sizeof(id));
  EepromWrite(EEPROM_OWN_RECORDS + offsetof(PeerRecords, deviceId), id,
              sizeof(id));
}
