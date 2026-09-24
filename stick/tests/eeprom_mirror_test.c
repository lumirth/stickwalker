#include "application/pw_eeprom_m95512.h"
#include "eeprom_map.h"
#include "project.h"

#include <assert.h>
#include <string.h>

RuntimeState g_state;
static u8 image[65536];

void EepromWrite(u16 address, void *source, u16 length)
{
  memcpy(image + address, source, length);
}

void EepromRead(u16 address, void *destination, u16 length)
{
  memcpy(destination, image + address, length);
}

u8 EepromReadByte(u16 address) { return image[address]; }

u8 EepromWriteByte(u16 address, u8 value)
{
  image[address] = value;
  return g_state.events.byte;
}

int main(void)
{
  SaveData source = {0};
  SaveData restored = {0};
  memset(image, 0xff, sizeof(image));
  source.totalSteps = 0x12345678;
  source.elapsedHours = 0x01020304;
  source.rtcSeconds = 0xa1b2c3d4;
  source.days = 0x3344;
  source.watts = 0x5566;
  source.pokemonMinutes = 0x7788;
  source.bonusCourse = 1;
  source.volume = 2;
  source.contrast = 4;

  EepromMirrorWrite(EEPROM_SAVE_PRIMARY, EEPROM_SAVE_BACKUP,
                    (u8 *)&source, sizeof(source));
  assert(image[EEPROM_SAVE_PRIMARY] == 0x12);
  assert(image[EEPROM_SAVE_PRIMARY + 1] == 0x34);
  assert(image[EEPROM_SAVE_PRIMARY + 2] == 0x56);
  assert(image[EEPROM_SAVE_PRIMARY + 3] == 0x78);
  assert(image[EEPROM_SAVE_PRIMARY + 12] == 0x33);
  assert(image[EEPROM_SAVE_PRIMARY + 13] == 0x44);
  assert(image[EEPROM_SAVE_PRIMARY + SAVE_SETTINGS_OFFSET] == 0x25);
  assert(memcmp(image + EEPROM_SAVE_PRIMARY,
                image + EEPROM_SAVE_BACKUP, sizeof(source) + 1) == 0);

  EepromMirrorRead(EEPROM_SAVE_PRIMARY, EEPROM_SAVE_BACKUP,
                   (u8 *)&restored, sizeof(restored));
  assert(restored.totalSteps == source.totalSteps);
  assert(restored.elapsedHours == source.elapsedHours);
  assert(restored.rtcSeconds == source.rtcSeconds);
  assert(restored.days == source.days);
  assert(restored.watts == source.watts);
  assert(restored.pokemonMinutes == source.pokemonMinutes);
  assert(restored.bonusCourse == source.bonusCourse);
  assert(restored.volume == source.volume);
  assert(restored.contrast == source.contrast);
  return 0;
}
