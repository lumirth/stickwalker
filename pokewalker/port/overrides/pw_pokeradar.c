#include "overrides.h"
#include "project.h"
#include "application/pw_eeprom_m95512.h"
#include "support/lib_common.h"
#include "support/scratch.h"

/* From upstream/src/application/pw_pokeradar.c.
 * DS encounter thresholds are already little-endian on this target. */
void pw_port_RadarChooseEncounter(void)
{
  u8 receiptIndex;
  DeviceStatus *buffer;
  EncounterRule *rule;
  u8 eventPresence;
  u32 randomValue;
  u8 chanceRoll;
  Course *course;
  u8 index;

  g_ui.view.radar.encounter = 0;
  if (g_state.save.bonusCourse != 0) {
    receiptIndex = EepromReadByte(PW_EEPROM_MEMBER_ADDRESS(
        EEPROM_BONUS_COURSE, BonusResources, values.pokemonReceipt));
    ScratchReset();
    buffer = ScratchAlloc(sizeof(DeviceStatus));
    if (StatusLoadReceived(buffer, receiptIndex) == 0) {
      eventPresence = EepromReadByte(EEPROM_EVENTS);
      if ((eventPresence & EVENT_PRESENT_POKEMON) == 0) {
        ScratchReset();
        rule = ScratchAlloc(sizeof(EncounterRule));
        EepromRead(PW_EEPROM_MEMBER_ADDRESS(EEPROM_BONUS_COURSE, BonusResources,
                                            values.encounterRule),
                   rule, sizeof(*rule));
        if (g_state.dailySteps >= rule->stepsLe) {
          randomValue = RandomNext() % 100ul;
          if (rule->chance > randomValue) {
            g_ui.view.radar.encounter = 4;
            randomValue = RandomNext();
            g_ui.view.radar.roundCount = (((randomValue >> 3) & 1) + 3);
            return;
          }
        }
      }
    }
  }

  ScratchReset();
  /* This inventory read has no consumer before Course replaces it in scratch.
   */
  buffer = ScratchAlloc(sizeof(((WalkData *)0)->pokemon));
  EepromRead(PW_EEPROM_MEMBER_ADDRESS(EEPROM_WALK, WalkData, pokemon), buffer,
             sizeof(((WalkData *)0)->pokemon));
  chanceRoll = (RandomNext() % 100ul);
  ScratchReset();
  course = ScratchAlloc(sizeof(Course));
  EepromRead(PW_EEPROM_MEMBER_ADDRESS(EEPROM_COURSE, CourseResources, values),
             course, sizeof(Course));
  index = 0;
  while (index < 3) {
    if (g_state.dailySteps >= course->encounterStepsLe[index]) {
      if (chanceRoll < course->encounterChance[index]) {
        g_ui.view.radar.encounter = (index + 1);
        randomValue = RandomNext();

        {
          /* Course indices 0, 1, 2 require 3-4, 2-3, 1-2 correct patches. */
          u8 draw = ((randomValue >> 3) & 1);
          index = -index;
          index += draw;
          index += 3;
        }
        g_ui.view.radar.roundCount = index;
        return;
      }
    }
    index++;
  }
  g_ui.view.radar.encounter = 3;
  randomValue = RandomNext();
  g_ui.view.radar.roundCount = (((randomValue >> 3) & 1) + 1);
}

extern __typeof__(pw_port_RadarChooseEncounter) RadarChooseEncounter
    __attribute__((alias("pw_port_RadarChooseEncounter")));
