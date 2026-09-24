#include "stick_wire_endian.h"

#include <assert.h>

int main(void)
{
  u8 bytes[4];
  StickWriteBe32(bytes, 0x1234abcd);
  assert(bytes[0] == 0x12 && bytes[1] == 0x34);
  assert(bytes[2] == 0xab && bytes[3] == 0xcd);
  assert(StickReadBe32(bytes) == 0x1234abcd);
  StickWriteBe16(bytes, 0x5678);
  assert(bytes[0] == 0x56 && bytes[1] == 0x78);
  return 0;
}
