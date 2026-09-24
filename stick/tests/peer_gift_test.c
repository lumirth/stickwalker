#include "application/pw_friend.h"
#include "application/pw_eeprom_m95512.h"
#include "eeprom_address.h"
#include "project.h"
#include "stick_wire_endian.h"

#include <assert.h>
#include <string.h>

RuntimeState g_state;
PresentationState g_ui;
static u8 scratch[2048];
static u16 scratchUsed;
static PeerInfo incoming;
static Item inventory[10];
static Item savedInventory[10];
static u8 expectedItemIndex;

void PeerAwardGift(void);

void ScratchReset(void) { scratchUsed = 0; }

void *ScratchAlloc(u16 size)
{
  void *result = scratch + scratchUsed;
  assert(scratchUsed + size <= sizeof(scratch));
  scratchUsed += size;
  return result;
}

void EepromRead(u16 address, void *destination, u16 length)
{
  if (address == EEPROM_PEER_INFO) {
    assert(length == sizeof(incoming));
    memcpy(destination, &incoming, length);
  } else {
    assert(address == PW_EEPROM_MEMBER_ADDRESS(EEPROM_WALK, WalkData,
                                                friendItems));
    assert(length == sizeof(inventory));
    memcpy(destination, inventory, length);
  }
}

void EepromWrite(u16 address, void *source, u16 length)
{
  assert(address == PW_EEPROM_MEMBER_ADDRESS(EEPROM_WALK, WalkData,
                                              friendItems));
  assert(length == sizeof(savedInventory));
  memcpy(savedInventory, source, length);
}

u16 CourseLoadItemIdLe(u8 index)
{
  assert(index == expectedItemIndex);
  return 0x1234;
}

void WattsAdd(u16 watts)
{
  (void)watts;
  assert(0 && "A free gift slot must receive an item");
}

int main(void)
{
  g_state.dailySteps = 1000;
  g_state.hourSteps = 2;
  expectedItemIndex = 5;
  StickWriteBe32((u8 *)&incoming.dailySteps, 5000);
  StickWriteBe16((u8 *)&incoming.hourSteps, 3);
  PeerAwardGift();

  /* 1000 + 5000 + (2 + 3) * 10 belongs to the 5000-step tier. The peer
   * has more daily steps, so the original source selects odd item index 5. */
  assert(g_ui.view.peer.itemIndex == 5);
  assert(g_ui.view.peer.wattsAwarded == 0);
  assert(savedInventory[0].idLe == 0x1234);

  memset(&g_ui, 0, sizeof(g_ui));
  memset(savedInventory, 0, sizeof(savedInventory));
  g_state.dailySteps = 1000;
  g_state.hourSteps = 6000;
  StickWriteBe32((u8 *)&incoming.dailySteps, 500);
  StickWriteBe16((u8 *)&incoming.hourSteps, 1000);
  expectedItemIndex = 4;
  PeerAwardGift();
  /* The H8's 16-bit hourly product is 70000 mod 65536 = 4464. Together
   * with 1500 daily steps this selects the 5000 tier, not the 20000 tier. */
  assert(g_ui.view.peer.itemIndex == 4);
  assert(savedInventory[0].idLe == 0x1234);
  return 0;
}
