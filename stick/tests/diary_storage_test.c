#include "application/pw_diary.h"
#include "application/pw_eeprom_m95512.h"
#include "project.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

RuntimeState g_state;
static DiaryEntry stored;

u8 EepromReadByte(u16 address)
{
  (void)address;
  return 0;
}

void EepromRead(u16 address, void *destination, u16 length)
{
  (void)address;
  memset(destination, 0, length);
}

void EepromWrite(u16 address, void *source, u16 length)
{
  (void)address;
  assert(length == sizeof(stored));
  memcpy(&stored, source, length);
}

u8 EepromMirrorWrite(u16 primary, u16 backup, u8 *buffer, u16 length)
{
  (void)primary;
  (void)backup;
  (void)buffer;
  (void)length;
  return 0;
}

int main(void)
{
  Course course = {0};
  DiaryEntry diary = {0};
  const u8 *bytes;

  g_state.save.rtcSeconds = 0x12345678;
  g_state.hourSteps = 0x2345;
  g_state.dailySteps = 0x3456789a;
  diary.peerHourSteps = 0x4567;
  diary.peerDaySteps = 0x56789abc;
  diary.compatibilityLe = 0x12345678;
  DiaryAppend(&course, &diary, 1, 0, 0, 0); /* First item-gift action. */

  bytes = (const u8 *)&stored;
  assert(bytes[offsetof(DiaryEntry, rtcSeconds)] == 0x12);
  assert(bytes[offsetof(DiaryEntry, rtcSeconds) + 3] == 0x78);
  assert(bytes[offsetof(DiaryEntry, ownHourSteps)] == 0x23);
  assert(bytes[offsetof(DiaryEntry, ownHourSteps) + 1] == 0x45);
  assert(bytes[offsetof(DiaryEntry, peerHourSteps)] == 0x45);
  assert(bytes[offsetof(DiaryEntry, peerHourSteps) + 1] == 0x67);
  assert(bytes[offsetof(DiaryEntry, ownDaySteps)] == 0x34);
  assert(bytes[offsetof(DiaryEntry, ownDaySteps) + 3] == 0x9a);
  assert(bytes[offsetof(DiaryEntry, peerDaySteps)] == 0x56);
  assert(bytes[offsetof(DiaryEntry, peerDaySteps) + 3] == 0xbc);
  /* The source forwards fields suffixed Le without a byte swap. */
  assert(bytes[offsetof(DiaryEntry, compatibilityLe)] == 0x78);
  return 0;
}
