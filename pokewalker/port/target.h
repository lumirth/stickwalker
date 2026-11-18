#ifndef POKEWALKER_TARGET_H
#define POKEWALKER_TARGET_H

#include <stddef.h>
#include "eeprom_address.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "StickWalker record and bitfield layouts require a little-endian target"
#endif

/* Preload these definitions before upstream's quoted sibling includes. */
#include "include/flags.h"
#include "include/view.h"

#undef PW_EEPROM_MEMBER_ADDRESS
#define PW_EEPROM_MEMBER_ADDRESS(base, type, member) \
  ((u16)((base) + offsetof(type, member)))

#endif
