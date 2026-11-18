#include "overrides.h"
#include "project.h"
#include "application/pw_pack.h"
#include "application/pw_eeprom_m95512.h"
#include "support/scratch.h"

/* From upstream/src/application/pw_pack.c.
 * The H8 high-byte inventory flag is bit 9 of the native mask. */
void pw_port_InventoryMasks(u8 *entryMasksOut)
{
  Pokemon *pokemon;
  Item *item;
  u8 i;
  u8 eventPresence;

  ScratchReset();
  pokemon = ScratchAlloc((sizeof(Pokemon) * INVENTORY_SLOTS));
  item = ScratchAlloc((sizeof(Item) * (INVENTORY_SLOTS + FRIEND_ITEM_SLOTS)));

  i = 0;
  do {
    *(u16 *)(entryMasksOut + i * 2) = 0;
    i++;
  } while (i < 2);

  if (g_state.flags.bits.hasPokemon) {
    *(u16 *)entryMasksOut |= WALK_MASK_HELD_POKEMON;
  }

  EepromRead(PW_EEPROM_MEMBER_ADDRESS(EEPROM_WALK, WalkData, pokemon), pokemon,
             (sizeof(Pokemon) * INVENTORY_SLOTS));
  EepromRead(PW_EEPROM_MEMBER_ADDRESS(EEPROM_WALK, WalkData, items), item,
             (sizeof(Item) * (INVENTORY_SLOTS + FRIEND_ITEM_SLOTS)));

  i = 0;
  do {
    if (pokemon[i].idLe != 0) {
      *(u16 *)entryMasksOut |= (WALK_MASK_CAUGHT_FIRST << i);
    }
    i++;
  } while (i < INVENTORY_SLOTS);

  i = 0;
  do {
    if (item[i].idLe != 0) {
      *(u16 *)entryMasksOut |= (WALK_MASK_ITEM_FIRST << i);
    }
    i++;
  } while (i < INVENTORY_SLOTS);

  item += INVENTORY_SLOTS;
  i = 0;
  do {
    if (item[i].idLe != 0) {
      *(u16 *)(entryMasksOut + 2) |= (1 << i);
    }
    i++;
  } while (i < FRIEND_ITEM_SLOTS);

  eventPresence = EepromReadByte(EEPROM_EVENTS);
  if ((eventPresence & EVENT_PRESENT_POKEMON) != 0) {
    *(u16 *)entryMasksOut |= WALK_MASK_EVENT_POKEMON;
  }
  if ((eventPresence & EVENT_PRESENT_MAP) != 0) {
    *(u16 *)entryMasksOut |= WALK_MASK_EVENT_MAP;
  }
  if ((eventPresence & EVENT_PRESENT_ITEM) != 0) {
    /* Set entry 9 in the native 16-bit mask. */
    *(u16 *)entryMasksOut |= (u16)WALK_MASK_EVENT_ITEM_HIGH_BYTE << 8;
  }
}

extern __typeof__(pw_port_InventoryMasks) InventoryMasks
    __attribute__((alias("pw_port_InventoryMasks")));
