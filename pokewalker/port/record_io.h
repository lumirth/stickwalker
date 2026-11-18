#ifndef POKEWALKER_RECORD_IO_H
#define POKEWALKER_RECORD_IO_H

#include "records.h"

void pw_eeprom_read_raw(u16 address, void *out, u16 length);
void pw_eeprom_write_raw(u16 address, const void *bytes, u16 length);
void pw_status_swap(DeviceStatus *status);
void pw_peer_swap(PeerInfo *peer);
void pw_mirror_swap(u16 primary_address, void *record, u16 length);

static inline u32 pw_read_be32(const u8 *bytes)
{
  return ((u32)bytes[0] << 24) | ((u32)bytes[1] << 16) | ((u32)bytes[2] << 8) |
         bytes[3];
}

static inline void pw_write_be32(u8 *bytes, u32 value)
{
  bytes[0] = value >> 24;
  bytes[1] = value >> 16;
  bytes[2] = value >> 8;
  bytes[3] = value;
}

#endif
