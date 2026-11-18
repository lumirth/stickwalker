#include "overrides.h"
#include "project.h"
#include "application/pw_eeprom_m95512.h"
#include "support/lib_common.h"
#include "support/scratch.h"

/* From upstream/src/application/pw_friend.c.
 * The hourly contribution wraps to 16 bits before joining the daily totals. */
void pw_port_PeerAwardGift(void)
{
  PeerInfo *peer;
  Item *items;
  u32 giftScore;
  u8 freeSlot;

  ScratchReset();
  /* Reserve the prefix for CourseLoadItemIdLe's scratch reset and course
   * reload. Use the peer and item bytes beyond it before another scratch
   * allocation.
   */
  ScratchAlloc(sizeof(Course));
  peer = ScratchAlloc(sizeof(PeerInfo));
  EepromRead(EEPROM_PEER_INFO, peer, sizeof(PeerInfo));
  items = ScratchAlloc(sizeof(Item) * 10);
  EepromRead(PW_EEPROM_MEMBER_ADDRESS(EEPROM_WALK, WalkData, friendItems),
             items, sizeof(Item) * 10);
  giftScore = g_state.dailySteps + peer->dailySteps +
              (u16)((g_state.hourSteps + peer->hourSteps) * 10u);
  if (giftScore > 20000ul) {
    giftScore = 20000ul;
  }
  freeSlot = 0;
  do {
    if (items[freeSlot].idLe == 0) {
      break;
    }
    freeSlot++;
  } while (freeSlot < 10);
  g_ui.view.peer.wattsAwarded = 0;
  if (freeSlot >= 10) {
    if ((g_ui.view.peer.wattsAwarded = (giftScore / 200ul)) == 0) {
      g_ui.view.peer.wattsAwarded = 1;
    }
    if (g_ui.view.peer.wattsAwarded > 99) {
      g_ui.view.peer.wattsAwarded = 99;
    }
    WattsAdd(g_ui.view.peer.wattsAwarded);
  }
  if (giftScore >= 20000ul) {
    g_ui.view.peer.giftMessageId = MESSAGE_PEER_PLAY_FIVE_NOTES;
    if (g_ui.view.peer.wattsAwarded != 0) {
      return;
    }
    if (g_state.dailySteps > peer->dailySteps) {
      g_ui.view.peer.itemIndex = 0;
    } else {
      g_ui.view.peer.itemIndex = 1;
    }
    items[freeSlot].idLe = CourseLoadItemIdLe(g_ui.view.peer.itemIndex);
    EepromWrite(PW_EEPROM_MEMBER_ADDRESS(EEPROM_WALK, WalkData, friendItems),
                items, sizeof(Item) * 10);
  } else if (giftScore >= 10000ul) {
    g_ui.view.peer.giftMessageId = MESSAGE_PEER_PLAY_FOUR_NOTES;
    if (g_ui.view.peer.wattsAwarded != 0) {
      return;
    }
    if (g_state.dailySteps > peer->dailySteps) {
      g_ui.view.peer.itemIndex = 2;
    } else {
      g_ui.view.peer.itemIndex = 3;
    }
    items[freeSlot].idLe = CourseLoadItemIdLe(g_ui.view.peer.itemIndex);
    EepromWrite(PW_EEPROM_MEMBER_ADDRESS(EEPROM_WALK, WalkData, friendItems),
                items, sizeof(Item) * 10);
  } else if (giftScore >= 5000ul) {
    g_ui.view.peer.giftMessageId = MESSAGE_PEER_PLAY_THREE_NOTES;
    if (g_ui.view.peer.wattsAwarded != 0) {
      return;
    }
    if (g_state.dailySteps > peer->dailySteps) {
      g_ui.view.peer.itemIndex = 4;
    } else {
      g_ui.view.peer.itemIndex = 5;
    }
    items[freeSlot].idLe = CourseLoadItemIdLe(g_ui.view.peer.itemIndex);
    EepromWrite(PW_EEPROM_MEMBER_ADDRESS(EEPROM_WALK, WalkData, friendItems),
                items, sizeof(Item) * 10);
  } else if (giftScore >= 2500ul) {
    g_ui.view.peer.giftMessageId = MESSAGE_PEER_PLAY_TWO_NOTES;
    if (g_ui.view.peer.wattsAwarded != 0) {
      return;
    }
    if (g_state.dailySteps > peer->dailySteps) {
      g_ui.view.peer.itemIndex = 6;
    } else {
      g_ui.view.peer.itemIndex = 7;
    }
    items[freeSlot].idLe = CourseLoadItemIdLe(g_ui.view.peer.itemIndex);
    EepromWrite(PW_EEPROM_MEMBER_ADDRESS(EEPROM_WALK, WalkData, friendItems),
                items, sizeof(Item) * 10);
  } else {
    g_ui.view.peer.giftMessageId = MESSAGE_PEER_PLAY_ONE_NOTE;
    if (g_ui.view.peer.wattsAwarded != 0) {
      return;
    }
    if (g_state.dailySteps > peer->dailySteps) {
      g_ui.view.peer.itemIndex = 8;
    } else {
      g_ui.view.peer.itemIndex = 9;
    }
    items[freeSlot].idLe = CourseLoadItemIdLe(g_ui.view.peer.itemIndex);
    EepromWrite(PW_EEPROM_MEMBER_ADDRESS(EEPROM_WALK, WalkData, friendItems),
                items, sizeof(Item) * 10);
  }
}

extern __typeof__(pw_port_PeerAwardGift) PeerAwardGift
    __attribute__((alias("pw_port_PeerAwardGift")));
