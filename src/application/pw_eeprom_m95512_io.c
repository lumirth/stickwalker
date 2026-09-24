#include "types.h"
#include "startup/hardware.h"
#include "project.h"
#include "application/pw_eeprom_m95512.h"
#include "support/lib_common.h"
#ifdef PW_STICK_S3
#include "stick_wire_endian.h"
#include "../../stick/eeprom_backend.h"
#include <string.h>
#endif

#ifdef PW_STICK_S3
/* Keep the C++ storage driver independent of H8-sized C records and the
 * ESP32 C++ library's incompatible uint typedef. */
void StickEepromSetError(u8 value)
{
  g_state.events.bits.eepromError = value;
}

u8 StickEepromEventByte(void)
{
  return g_state.events.byte;
}
#endif

#ifndef PW_STICK_S3
#pragma inline(EepromWaitReady)
static void EepromWaitReady(void)
{
  while (SSU.SSSR.BIT.TDRE == 0) {
  }
}
#endif

#define MIRROR_NONE_VALID 0
#define MIRROR_PRIMARY_VALID 1
#define MIRROR_BACKUP_VALID 2

/* Write the payload to primary then backup. Each copy ends with a checksum byte
 * initialized to 1 and incremented by each payload byte. */
u8 EepromMirrorWrite(u16 primary, u16 backup, u8 *buffer, u16 length)
{
  u8 checksum;
  u8 i;
#ifdef PW_STICK_S3
  u8 saveBytes[sizeof(SaveData)];
  u8 *serialized = buffer;

  if (primary == EEPROM_SAVE_PRIMARY && backup == EEPROM_SAVE_BACKUP &&
      length == sizeof(SaveData)) {
    const SaveData *save = (const SaveData *)buffer;
    memcpy(saveBytes, buffer, length);
    StickWriteBe32(saveBytes + offsetof(SaveData, totalSteps),
                   save->totalSteps);
    StickWriteBe32(saveBytes + offsetof(SaveData, elapsedHours),
                   save->elapsedHours);
    StickWriteBe32(saveBytes + offsetof(SaveData, rtcSeconds),
                   save->rtcSeconds);
    StickWriteBe16(saveBytes + offsetof(SaveData, days), save->days);
    StickWriteBe16(saveBytes + offsetof(SaveData, watts), save->watts);
    StickWriteBe16(saveBytes + offsetof(SaveData, pokemonMinutes),
                   save->pokemonMinutes);
    serialized = saveBytes;
  }
  buffer = serialized;
#endif

  checksum = 1;
#ifdef PW_STICK_S3
  StickEepromBatchBegin();
#endif
  EepromWrite(primary, buffer, length);
  i = 0;
  while (i < length) {
    checksum = (checksum + buffer[i]);
    i++;
  }
  EepromWriteByte((primary + length), checksum);
  EepromWrite(backup, buffer, length);
#ifdef PW_STICK_S3
  EepromWriteByte((backup + length), checksum);
  StickEepromBatchEnd();
  return StickEepromEventByte();
#else
  return EepromWriteByte((backup + length), checksum);
#endif
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

  EepromRead(backup, buffer, length);
  checksum[1] = 1;
  i = 0;
  while (i < length) {
    checksum[1] = (checksum[1] + buffer[i]);
    i++;
  }
  EepromRead(primary, buffer, length);
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
    EepromWrite(primary, buffer, length);
    EepromWriteByte(primaryCsAddr, 0xff);
    EepromWrite(backup, buffer, length);
    EepromWriteByte(backupCsAddr, 0xff);
    break;
  case MIRROR_PRIMARY_VALID:
    EepromRead(primary, buffer, length);
    EepromWrite(backup, buffer, length);
    EepromWriteByte(backupCsAddr, checksum[0]);
    break;
  case MIRROR_BACKUP_VALID:
    EepromRead(backup, buffer, length);
    EepromWrite(primary, buffer, length);
    EepromWriteByte(primaryCsAddr, checksum[1]);
    break;
  case MIRROR_PRIMARY_VALID | MIRROR_BACKUP_VALID:
    if (checksum[0] != checksum[1]) {
      EepromWrite(backup, buffer, length);
      EepromWriteByte(backupCsAddr, checksum[0]);
    }
    break;
  }
#ifdef PW_STICK_S3
  if (primary == EEPROM_SAVE_PRIMARY && backup == EEPROM_SAVE_BACKUP &&
      length == sizeof(SaveData)) {
    SaveData *save = (SaveData *)buffer;
    u8 *raw = buffer;
    u32 total = StickReadBe32(raw + offsetof(SaveData, totalSteps));
    u32 hours = StickReadBe32(raw + offsetof(SaveData, elapsedHours));
    u32 seconds = StickReadBe32(raw + offsetof(SaveData, rtcSeconds));
    u16 days = StickReadBe16(raw + offsetof(SaveData, days));
    u16 watts = StickReadBe16(raw + offsetof(SaveData, watts));
    u16 minutes = StickReadBe16(raw + offsetof(SaveData, pokemonMinutes));
    save->totalSteps = total;
    save->elapsedHours = hours;
    save->rtcSeconds = seconds;
    save->days = days;
    save->watts = watts;
    save->pokemonMinutes = minutes;
  }
#endif
}

/* Page writes wrap inside the EEPROM page, so split an arbitrary span at each
 * boundary. An overrun stays latched across retries of this whole operation. */
#ifndef PW_STICK_S3
void EepromWrite(u16 address, void *source, u16 length)
{
  u8 attempts;
  u8 status;
  u16 workingAddr;
  u16 workingCount;
  u8 *workingSrc;

  attempts = EEPROM_TRANSFER_ATTEMPTS;
  g_state.events.bits.eepromError = 0;
  while (attempts != 0) {
    WatchdogService();
    workingSrc = source;
    workingAddr = address;
    workingCount = length;
    EepromConfigure();
    while (workingCount != 0) {
      SSU.SSER.BYTE &= SSU_TX_RX_CLEAR;
      SSU.SSSR.BYTE = 0;
      SSU.SSER.BYTE |= SSU_TX_RX_ENABLE;
      IO.PDR1.BIT.B2 = 0;
      EepromWaitReady();
      SSU.SSTDR = EEPROM_CMD_READ_STATUS;
      EepromReceive();
      do {
        EepromWaitReady();
        SSU.SSTDR = 0xff;
        status = EepromReceive();
        status &= 1;
      } while (status == 1);
      while (SSU.SSSR.BIT.TEND == 0) {
      }
      IO.PDR1.BIT.B2 = 1;
      SSU.SSER.BYTE &= SSU_TX_RX_CLEAR;
      SSU.SSSR.BYTE = 0;
      SSU.SSER.BIT.TE = 1;
      IO.PDR1.BIT.B2 = 0;
      EepromWaitReady();
      SSU.SSTDR = EEPROM_CMD_WRITE_ENABLE;
      while (SSU.SSSR.BIT.TEND == 0) {
      }
      IO.PDR1.BIT.B2 = 1;
      IO.PDR1.BIT.B2 = 0;
      EepromWaitReady();
      SSU.SSTDR = EEPROM_CMD_PAGE_PROGRAM;
      status = (workingAddr >> 8);
      EepromWaitReady();
      SSU.SSTDR = status;
      status = workingAddr;
      EepromWaitReady();
      SSU.SSTDR = status;
      status = 0;
      do {
        {
          u8 tx;
          tx = *workingSrc;
          EepromWaitReady();
          SSU.SSTDR = tx;
        }
        workingSrc++;
        workingAddr++;
        workingCount--;
        if ((workingAddr & EEPROM_PAGE_OFFSET_MASK) == 0) {
          break;
        }
        if (workingCount == 0) {
          break;
        }
        status++;
      } while (status < EEPROM_PAGE_BYTES);
      while (SSU.SSSR.BIT.TEND == 0) {
      }
      IO.PDR1.BIT.B2 = 1;
    }
    EepromIdle();
    if (g_state.events.bits.eepromError == 0) {
      break;
    }
    attempts--;
  }
}

/* Read length consecutive bytes. The command and address responses first write
 * destination[0], so the buffer requires a writable byte even at length zero.
 * Payload bytes then overwrite the buffer from its beginning. */
void EepromRead(u16 address, void *destination, u16 length)
{
  u8 attempts;
  u8 status;
  u16 workingAddr;
  u16 workingCount;
  u8 *workingDest;

  attempts = EEPROM_TRANSFER_ATTEMPTS;
  g_state.events.bits.eepromError = 0;
  while (attempts != 0) {
    WatchdogService();
    workingDest = destination;
    workingAddr = address;
    workingCount = length;
    EepromConfigure();
    SSU.SSER.BYTE &= SSU_TX_RX_CLEAR;
    SSU.SSSR.BYTE = 0;
    SSU.SSER.BYTE |= SSU_TX_RX_ENABLE;
    IO.PDR1.BIT.B2 = 0;
    EepromWaitReady();
    SSU.SSTDR = EEPROM_CMD_READ_STATUS;
    EepromReceive();
    do {
      EepromWaitReady();
      SSU.SSTDR = 0xff;
      status = EepromReceive();
      status &= 1;
    } while (status == 1);
    while (SSU.SSSR.BIT.TEND == 0) {
    }
    IO.PDR1.BIT.B2 = 1;
    SSU.SSER.BYTE &= SSU_TX_RX_CLEAR;
    SSU.SSSR.BYTE = 0;
    SSU.SSER.BYTE |= SSU_TX_RX_ENABLE;
    IO.PDR1.BIT.B2 = 0;
    EepromWaitReady();
    SSU.SSTDR = EEPROM_CMD_READ;
    for (;;) {
      if (SSU.SSSR.BIT.ORER) {
        SSU.SSSR.BIT.ORER = 0;
        g_state.events.bits.eepromError = 1;
        break;
      }
      if (SSU.SSSR.BIT.RDRF) {
        break;
      }
    }
    *workingDest = SSU.SSRDR;
    status = (workingAddr >> 8);
    EepromWaitReady();
    SSU.SSTDR = status;
    for (;;) {
      if (SSU.SSSR.BIT.ORER) {
        SSU.SSSR.BIT.ORER = 0;
        g_state.events.bits.eepromError = 1;
        break;
      }
      if (SSU.SSSR.BIT.RDRF) {
        break;
      }
    }
    *workingDest = SSU.SSRDR;
    status = workingAddr;
    EepromWaitReady();
    SSU.SSTDR = status;
    for (;;) {
      if (SSU.SSSR.BIT.ORER) {
        SSU.SSSR.BIT.ORER = 0;
        g_state.events.bits.eepromError = 1;
        break;
      }
      if (SSU.SSSR.BIT.RDRF) {
        break;
      }
    }
    *workingDest = SSU.SSRDR;
    while (workingCount != 0) {
      EepromWaitReady();
      SSU.SSTDR = 0xff;
      for (;;) {
        if (SSU.SSSR.BIT.ORER) {
          SSU.SSSR.BIT.ORER = 0;
          g_state.events.bits.eepromError = 1;
          break;
        }
        if (SSU.SSSR.BIT.RDRF) {
          break;
        }
      }
      *workingDest = SSU.SSRDR;
      workingDest++;
      workingAddr++;
      workingCount--;
    }
    while (SSU.SSSR.BIT.TEND == 0) {
    }
    IO.PDR1.BIT.B2 = 1;
    EepromIdle();
    if (g_state.events.bits.eepromError == 0) {
      break;
    }
    attempts--;
  }
}

/* Read one byte with the receiver enabled throughout the command, address and
 * data transfers. */
u8 EepromReadByte(u16 address)
{
  u8 attempts;
  u8 status;
  u16 addrHi;
  u8 byte;

  attempts = EEPROM_TRANSFER_ATTEMPTS;
  g_state.events.bits.eepromError = 0;
  addrHi = (address >> 8);
  while (attempts != 0) {
    WatchdogService();
    EepromConfigure();
    SSU.SSER.BYTE &= SSU_TX_RX_CLEAR;
    SSU.SSSR.BYTE = 0;
    SSU.SSER.BYTE |= SSU_TX_RX_ENABLE;
    IO.PDR1.BIT.B2 = 0;
    EepromWaitReady();
    SSU.SSTDR = EEPROM_CMD_READ_STATUS;
    EepromReceive();
    do {
      EepromWaitReady();
      SSU.SSTDR = 0xff;
      status = EepromReceive();
      status &= 1;
    } while (status == 1);
    while (SSU.SSSR.BIT.TEND == 0) {
    }
    IO.PDR1.BIT.B2 = 1;
    SSU.SSER.BYTE &= SSU_TX_RX_CLEAR;
    SSU.SSSR.BYTE = 0;
    SSU.SSER.BYTE |= SSU_TX_RX_ENABLE;
    IO.PDR1.BIT.B2 = 0;
    EepromWaitReady();
    SSU.SSTDR = EEPROM_CMD_READ;
    EepromReceive();
    EepromWaitReady();
    SSU.SSTDR = addrHi;
    EepromReceive();
    byte = address;
    EepromWaitReady();
    SSU.SSTDR = byte;
    EepromReceive();
    EepromWaitReady();
    SSU.SSTDR = 0xff;
    byte = EepromReceive();
    while (SSU.SSSR.BIT.TEND == 0) {
    }
    IO.PDR1.BIT.B2 = 1;
    EepromIdle();
    if (g_state.events.bits.eepromError == 0) {
      break;
    }
    attempts--;
  }
  return byte;
}

/* Fill a 128-byte EEPROM page. The caller owns watchdog service; each retry
 * reconfigures the SSU before sending the command. */
void EepromFillPage(u16 address, u8 byteValue)
{
  u8 addrHi;
  u8 status;
  u8 attempts;
  u8 remaining;

  attempts = EEPROM_TRANSFER_ATTEMPTS;
  g_state.events.bits.eepromError = 0;
  addrHi = (address >> 8);
  while (attempts != 0) {
    EepromConfigure();
    SSU.SSER.BYTE &= SSU_TX_RX_CLEAR;
    SSU.SSSR.BYTE = 0;
    SSU.SSER.BYTE |= SSU_TX_RX_ENABLE;
    IO.PDR1.BIT.B2 = 0;
    EepromWaitReady();
    SSU.SSTDR = EEPROM_CMD_READ_STATUS;
    EepromReceive();
    do {
      EepromWaitReady();
      SSU.SSTDR = 0xff;
      status = EepromReceive();
      status &= 1;
    } while (status == 1);
    while (SSU.SSSR.BIT.TEND == 0) {
    }
    IO.PDR1.BIT.B2 = 1;
    SSU.SSER.BYTE &= SSU_TX_RX_CLEAR;
    SSU.SSSR.BYTE = 0;
    SSU.SSER.BIT.TE = 1;
    IO.PDR1.BIT.B2 = 0;
    EepromWaitReady();
    SSU.SSTDR = EEPROM_CMD_WRITE_ENABLE;
    while (SSU.SSSR.BIT.TEND == 0) {
    }
    IO.PDR1.BIT.B2 = 1;
    IO.PDR1.BIT.B2 = 0;
    EepromWaitReady();
    SSU.SSTDR = EEPROM_CMD_PAGE_PROGRAM;
    status = addrHi;
    EepromWaitReady();
    SSU.SSTDR = status;
    status = address;
    EepromWaitReady();
    SSU.SSTDR = status;
    {
      u8 tx;
      remaining = EEPROM_PAGE_BYTES;
      do {
        tx = byteValue;
        EepromWaitReady();
        SSU.SSTDR = tx;
        remaining--;
      } while (remaining != 0);
    }
    while (SSU.SSSR.BIT.TEND == 0) {
    }
    IO.PDR1.BIT.B2 = 1;
    EepromIdle();
    if (g_state.events.bits.eepromError == 0) {
      break;
    }
    attempts--;
  }
}

/* Fill count bytes with value, splitting writes at 128-byte page boundaries. */
void EepromFill(u16 address, u16 count, u8 value)
{
  u8 attempts;
  u8 status;
  u16 workingAddr;
  u16 workingCount;

  attempts = EEPROM_TRANSFER_ATTEMPTS;
  g_state.events.bits.eepromError = 0;
  while (attempts != 0) {
    WatchdogService();
    workingAddr = address;
    workingCount = count;
    EepromConfigure();
    while (workingCount != 0) {
      SSU.SSER.BYTE &= SSU_TX_RX_CLEAR;
      SSU.SSSR.BYTE = 0;
      SSU.SSER.BYTE |= SSU_TX_RX_ENABLE;
      IO.PDR1.BIT.B2 = 0;
      EepromWaitReady();
      SSU.SSTDR = EEPROM_CMD_READ_STATUS;
      EepromReceive();
      do {
        EepromWaitReady();
        SSU.SSTDR = 0xff;
        status = EepromReceive();
        status &= 1;
      } while (status == 1);
      while (SSU.SSSR.BIT.TEND == 0) {
      }
      IO.PDR1.BIT.B2 = 1;
      SSU.SSER.BYTE &= SSU_TX_RX_CLEAR;
      SSU.SSSR.BYTE = 0;
      SSU.SSER.BIT.TE = 1;
      IO.PDR1.BIT.B2 = 0;
      EepromWaitReady();
      SSU.SSTDR = EEPROM_CMD_WRITE_ENABLE;
      while (SSU.SSSR.BIT.TEND == 0) {
      }
      IO.PDR1.BIT.B2 = 1;
      IO.PDR1.BIT.B2 = 0;
      EepromWaitReady();
      SSU.SSTDR = EEPROM_CMD_PAGE_PROGRAM;
      status = (workingAddr >> 8);
      EepromWaitReady();
      SSU.SSTDR = status;
      status = workingAddr;
      EepromWaitReady();
      SSU.SSTDR = status;
      status = 0;
      do {
        {
          u8 tx;
          tx = value;
          EepromWaitReady();
          SSU.SSTDR = tx;
        }
        workingAddr++;
        workingCount--;
        if ((workingAddr & EEPROM_PAGE_OFFSET_MASK) == 0) {
          break;
        }
        if (workingCount == 0) {
          break;
        }
        status++;
      } while (status < EEPROM_PAGE_BYTES);
      while (SSU.SSSR.BIT.TEND == 0) {
      }
      IO.PDR1.BIT.B2 = 1;
    }
    EepromIdle();
    if (g_state.events.bits.eepromError == 0) {
      break;
    }
    attempts--;
  }
}

/* Page-program exactly 128 bytes from source. */
void EepromWritePage(uint address, u8 *source)
{
  u8 status;
  u16 workingAddr;
  u8 *workingSrc;
  u8 attempts;

  attempts = EEPROM_TRANSFER_ATTEMPTS;
  g_state.events.bits.eepromError = 0;
  while (attempts != 0) {
    WatchdogService();
    workingSrc = source;
    workingAddr = address;
    EepromConfigure();
    SSU.SSER.BYTE &= SSU_TX_RX_CLEAR;
    SSU.SSSR.BYTE = 0;
    SSU.SSER.BYTE |= SSU_TX_RX_ENABLE;
    IO.PDR1.BIT.B2 = 0;
    EepromWaitReady();
    SSU.SSTDR = EEPROM_CMD_READ_STATUS;
    EepromReceive();
    do {
      EepromWaitReady();
      SSU.SSTDR = 0xff;
      status = EepromReceive();
      status &= 1;
    } while (status == 1);
    while (SSU.SSSR.BIT.TEND == 0) {
    }
    IO.PDR1.BIT.B2 = 1;
    SSU.SSER.BYTE &= SSU_TX_RX_CLEAR;
    SSU.SSSR.BYTE = 0;
    SSU.SSER.BIT.TE = 1;
    IO.PDR1.BIT.B2 = 0;
    EepromWaitReady();
    SSU.SSTDR = EEPROM_CMD_WRITE_ENABLE;
    while (SSU.SSSR.BIT.TEND == 0) {
    }
    IO.PDR1.BIT.B2 = 1;
    IO.PDR1.BIT.B2 = 0;
    EepromWaitReady();
    SSU.SSTDR = EEPROM_CMD_PAGE_PROGRAM;
    status = (workingAddr >> 8);
    EepromWaitReady();
    SSU.SSTDR = status;
    status = workingAddr;
    EepromWaitReady();
    SSU.SSTDR = status;
    status = EEPROM_PAGE_BYTES;
    do {
      {
        u8 tx;
        tx = *workingSrc++;
        EepromWaitReady();
        SSU.SSTDR = tx;
      }
      status--;
    } while (status != 0);
    while (SSU.SSSR.BIT.TEND == 0) {
    }
    IO.PDR1.BIT.B2 = 1;
    EepromIdle();
    if (g_state.events.bits.eepromError == 0) {
      break;
    }
    attempts--;
  }
}
#endif
