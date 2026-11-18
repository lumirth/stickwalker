#include "overrides.h"
#include "project.h"
#include "application/pw_diary.h"
#include "application/pw_carry_overflow.h"
#include "application/pw_eeprom_m95512.h"
#include "support/lib_common.h"
#include "support/scratch.h"

void EventItemRead(u8 *destination);
void EventItemWrite(u8 *source);

#define DOWSING_BONUS_ITEM 10

/* From upstream/src/application/pw_dowsing.c.
 * DS item thresholds are already little-endian on this target. */
void pw_port_DowsingAwardBonus(void)
{
  DeviceStatus *status;
  EventItem *eventItem;
  BonusCourse *bonus;
  u8 *course;
  uint requiredSteps;
  u8 noEncounter;
  u8 flags;

  ScratchReset();
  status = ScratchAlloc(0x68);
  eventItem = ScratchAlloc(sizeof(EventItem));
  bonus = ScratchAlloc(sizeof(BonusCourse));
  EepromRead(EEPROM_BONUS_COURSE, bonus, sizeof(BonusCourse));
  requiredSteps = bonus->itemStepsLe;
  if (g_state.dailySteps < requiredSteps) {
    g_ui.view.dowsing.wattsAwarded = (g_ui.view.dowsing.attemptsLeft * 4 + 2);
    WattsAdd(g_ui.view.dowsing.wattsAwarded);
    return;
  }
  /* Reduce an eight-bit draw modulo 100 for the bonus-item chance. */
  if ((u8)(RandomNext() >> 3) % 100 >= bonus->itemChance) {
    g_ui.view.dowsing.wattsAwarded = (g_ui.view.dowsing.attemptsLeft * 4 + 2);
    WattsAdd(g_ui.view.dowsing.wattsAwarded);
    return;
  }
  EventItemRead((u8 *)eventItem);
  if (eventItem->header.itemIdLe != 0) {
    g_ui.view.dowsing.wattsAwarded = (g_ui.view.dowsing.attemptsLeft * 4 + 2);
    WattsAdd(g_ui.view.dowsing.wattsAwarded);
    return;
  }
  if (StatusLoadReceived(status, bonus->itemReceipt) != 0) {
    g_ui.view.dowsing.wattsAwarded = (g_ui.view.dowsing.attemptsLeft * 4 + 2);
    WattsAdd(g_ui.view.dowsing.wattsAwarded);
    return;
  }
  StatusSetReceived(status, bonus->itemReceipt);
  course = ScratchAlloc(sizeof(Course));
  EepromRead(EEPROM_COURSE, course, sizeof(Course));
  g_ui.view.dowsing.itemIndex = DOWSING_BONUS_ITEM;
  eventItem->header.prefix.transfer.word = bonus->itemPrefix.transfer.word;
  eventItem->header.prefix.transfer.halfword =
      bonus->itemPrefix.transfer.halfword;
  eventItem->header.itemIdLe = bonus->itemIdLe;
  EepromRead((u16)((BonusResources *)EEPROM_BONUS_COURSE)->itemName,
             eventItem->itemName, sizeof(eventItem->itemName));
  EventItemWrite((u8 *)eventItem);
  flags = (EepromReadByte(EEPROM_EVENTS) | EVENT_PRESENT_ITEM);
  EepromWriteByte(EEPROM_EVENTS, flags);
  if (g_state.flags.bits.hasPokemon == 0) {
    return;
  }
  noEncounter = 0;
  DiaryAppend((Course *)course, ScratchAlloc(sizeof(DiaryEntry)),
              PW_DIARY_ACTION_BONUS_COURSE_ITEM, 1, noEncounter,
              eventItem->header.itemIdLe);
}

extern __typeof__(pw_port_DowsingAwardBonus) DowsingAwardBonus
    __attribute__((alias("pw_port_DowsingAwardBonus")));

void pw_port_DowsingChooseReward(void)
{
  Item *items;
  Course *course;
  u8 itemIndex;

  ScratchReset();
  items = ScratchAlloc(sizeof(((WalkData *)0)->items));
  EepromRead(PW_EEPROM_MEMBER_ADDRESS(EEPROM_WALK, WalkData, items), items,
             sizeof(((WalkData *)0)->items));
  g_ui.view.dowsing.freeItemSlot = ItemSlotFindEmpty(items);
  if (g_state.save.bonusCourse != 0) {
    DowsingAwardBonus();
    return;
  }
  course = ScratchAlloc(sizeof(Course));
  EepromRead(EEPROM_COURSE, course, sizeof(Course));
  itemIndex = 0;
  do {
    uint itemSlot = itemIndex;

    if (g_state.dailySteps >= course->itemStepsLe[itemSlot]) {
      if ((u8)(RandomNext() >> 3) % 100 < course->itemChance[itemSlot]) {
        break;
      }
    }
    itemIndex++;
  } while (itemIndex < 10);
  if (itemIndex > 9) {
    itemIndex = 9;
  }
  g_ui.view.dowsing.itemIndex = itemIndex;
  g_ui.view.dowsing.itemIdLe = course->itemIdLe[itemIndex];
}

extern __typeof__(pw_port_DowsingChooseReward) DowsingChooseReward
    __attribute__((alias("pw_port_DowsingChooseReward")));
