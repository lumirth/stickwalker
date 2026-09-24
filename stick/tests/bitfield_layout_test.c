#include "flags.h"
#include "records.h"
#include "save.h"
#include "view.h"
#include "application/pw_follower_events.h"

#include <assert.h>
#include <string.h>

int main(void)
{
  SystemFlags system = {0};
  system.bits.registered = 1;
  system.bits.interactiveMode = 1;
  assert(system.byte == (SYSTEM_REGISTERED | SYSTEM_MODE_INTERACTIVE));

  SystemEvents events = {0};
  events.bits.eepromError = 1;
  events.bits.uiRefreshPending = 1;
  assert(events.byte == (EVENT_EEPROM_ERROR | EVENT_UI_REFRESH));

  HomeMotionFlags motion = {0};
  motion.bits.movingRight = 1;
  assert(motion.byte == 4);
  IrSessionFlags session = {0};
  session.bits.receivedBurst = 1;
  assert(session.byte == 1);
  IrRoleFlags role = {0};
  role.bits.peerStatusResponder = 1;
  assert(role.byte == 1);
  PeerFlags peer = {0};
  peer.bits.fixedFacing = 1;
  assert(peer.byte == 2);

  SocialFlags social = {0};
  social.bits.bubbleIndex = 5;
  social.bits.lowerMessageMode = 2;
  social.bits.showTreasure = 1;
  assert(social.byte == SOCIAL_FLAGS(5, 2, SOCIAL_SHOW_TREASURE));

  BattleFlags battle = {0};
  battle.bits.responseRow = 5;
  battle.bits.playerAction = 2;
  battle.bits.opponentHpVisible = 1;
  assert(battle.byte == (5 << 5) + (2 << 1) + 1);

  DeviceStatus status;
  memset(&status, 0, sizeof(status));
  status.registered = 1;
  status.rolloverHour = 3;
  assert(((unsigned char *)&status)[STATUS_FLAGS_OFFSET] == 0x19);

  SaveData save;
  memset(&save, 0, sizeof(save));
  save.bonusCourse = 1;
  save.volume = 2;
  save.contrast = 4;
  assert(((unsigned char *)&save)[SAVE_SETTINGS_OFFSET] == 0x25);
  return 0;
}
