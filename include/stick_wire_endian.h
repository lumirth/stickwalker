#ifndef PW_STICK_WIRE_ENDIAN_H
#define PW_STICK_WIRE_ENDIAN_H

#include "types.h"

/* H8-native multi-byte protocol fields are big-endian on the wire. Fields
 * documented with a Le suffix remain raw forwarded bytes instead. */
static inline u32 StickReadBe32(const u8 *bytes)
{
  return ((u32)bytes[0] << 24) | ((u32)bytes[1] << 16) |
         ((u32)bytes[2] << 8) | bytes[3];
}

static inline u16 StickReadBe16(const u8 *bytes)
{
  return ((u16)bytes[0] << 8) | bytes[1];
}

static inline void StickWriteBe16(u8 *bytes, u16 value)
{
  bytes[0] = (u8)(value >> 8);
  bytes[1] = (u8)value;
}

static inline void StickWriteBe32(u8 *bytes, u32 value)
{
  bytes[0] = (u8)(value >> 24);
  bytes[1] = (u8)(value >> 16);
  bytes[2] = (u8)(value >> 8);
  bytes[3] = (u8)value;
}

#endif
