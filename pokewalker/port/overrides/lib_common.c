#include "types.h"
#include <stddef.h>
#include "pokewalker_host.h"
#include "project.h"
#include "support/lib_common.h"
#include "support/scratch.h"

/* Native pointers for scratch storage and signed LZ back-references. */
static _Alignas(8) u8 scratch_buffer[2048];
void pw_port_ScratchReset(void)
{
  g_state.scratchCursor = 0;
}
extern __typeof__(pw_port_ScratchReset) ScratchReset
  __attribute__((alias("pw_port_ScratchReset")));

void *pw_port_ScratchAlloc(u16 byteCount)
{
  u16 at = g_state.scratchCursor;
  u32 next = (u32)at + byteCount;
  if (next > sizeof(scratch_buffer)) {
    pw_host_fault("Pokewalker scratch capacity exceeded");
    return scratch_buffer;
  }
  g_state.scratchCursor = (u16)((next + 3) & ~3u);
  return scratch_buffer + at;
}
extern __typeof__(pw_port_ScratchAlloc) ScratchAlloc
  __attribute__((alias("pw_port_ScratchAlloc")));

void pw_port_BulkDecode(u8 *packed, u8 *out)
{
  u8 remaining;
  u8 flags;
  s8 bits;
  u8 length;
  ptrdiff_t back;

  remaining = packed[1];
  packed += 4;
  while (remaining != 0) {
    flags = *packed++;
    bits = 8;
    while (--bits >= 0) {
      if ((flags & 0x80) == 0) {
        *out++ = *packed++;
        remaining--;
      } else {
        length = (*packed / 16);
        length += 3;
        packed++;
        back = (*packed++ + 1);
        remaining = (remaining - length);
        back = -back;
        do {
          *out = *(out + back);
          out++;
        } while (--length != 0);
      }
      if (remaining == 0) {
        break;
      }
      flags = (flags << 1);
    }
  }
}
extern __typeof__(pw_port_BulkDecode) BulkDecode
  __attribute__((alias("pw_port_BulkDecode")));
