#include "port_internal.h"
#include "types.h"
#include "pokewalker_host.h"
#include "record_io.h"
#include <string.h>
#include "project.h"
#include "application/pw_eeprom_m95512.h"
#include "support/lib_common.h"

#define MIRROR_NONE_VALID 0
#define MIRROR_PRIMARY_VALID 1
#define MIRROR_BACKUP_VALID 2

/* Write the payload to primary then backup. Each copy ends with a checksum byte
 * initialized to 1 and incremented by each payload byte. */
u8 EepromMirrorWrite(u16 primary, u16 backup, u8 *buffer, u16 length)
{
  u8 encoded[255];
  u8 checksum;
  u8 i;

  if (length > sizeof(encoded)) {
    pw_host_fault("EEPROM mirror exceeds its checksum cursor");
    return 0;
  }
  memcpy(encoded, buffer, length);
  pw_mirror_swap(primary, encoded, length);
  buffer = encoded;
  checksum = 1;
  pw_eeprom_write_raw(primary, buffer, length);
  i = 0;
  while (i < length) {
    checksum = (checksum + buffer[i]);
    i++;
  }
  EepromWriteByte((primary + length), checksum);
  pw_eeprom_write_raw(backup, buffer, length);
  return EepromWriteByte((backup + length), checksum);
}

/* Load a valid payload and repair a bad mirror. Read backup before primary;
 * when both validate, keep primary and rewrite backup if the checksums differ.
 * If neither validates, fill the buffer, both payloads and checksums with FF.
 */
void EepromMirrorRead(u16 primary, u16 backup, u8 *buffer, u16 length)
{
  u8 checksum[2];
  u8 i;
  u8 erased;
  u16 primaryCsAddr;
  u16 backupCsAddr;
  u8 valid;

  pw_eeprom_read_raw(backup, buffer, length);
  checksum[1] = 1;
  i = 0;
  while (i < length) {
    checksum[1] = (checksum[1] + buffer[i]);
    i++;
  }
  pw_eeprom_read_raw(primary, buffer, length);
  checksum[0] = 1;
  i = 0;
  while (i < length) {
    checksum[0] = (checksum[0] + buffer[i]);
    i++;
  }
  valid = MIRROR_NONE_VALID;
  primaryCsAddr = (primary + length);
  if (checksum[0] == EepromReadByte(primaryCsAddr)) {
    valid |= MIRROR_PRIMARY_VALID;
  }
  backupCsAddr = (backup + length);
  if (checksum[1] == EepromReadByte(backupCsAddr)) {
    valid |= MIRROR_BACKUP_VALID;
  }
  switch (valid) {
  case MIRROR_NONE_VALID:
    i = 0;
    erased = 0xff;
    while (i < length) {
      buffer[i] = erased;
      i++;
    }
    pw_eeprom_write_raw(primary, buffer, length);
    EepromWriteByte(primaryCsAddr, 0xff);
    pw_eeprom_write_raw(backup, buffer, length);
    EepromWriteByte(backupCsAddr, 0xff);
    break;
  case MIRROR_PRIMARY_VALID:
    pw_eeprom_read_raw(primary, buffer, length);
    pw_eeprom_write_raw(backup, buffer, length);
    EepromWriteByte(backupCsAddr, checksum[0]);
    break;
  case MIRROR_BACKUP_VALID:
    pw_eeprom_read_raw(backup, buffer, length);
    pw_eeprom_write_raw(primary, buffer, length);
    EepromWriteByte(primaryCsAddr, checksum[1]);
    break;
  case MIRROR_PRIMARY_VALID | MIRROR_BACKUP_VALID:
    if (checksum[0] != checksum[1]) {
      pw_eeprom_write_raw(backup, buffer, length);
      EepromWriteByte(backupCsAddr, checksum[0]);
    }
    break;
  }
  pw_mirror_swap(primary, buffer, length);
}
