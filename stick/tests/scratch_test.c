#include "project.h"
#include "support/scratch.h"

#include <assert.h>

RuntimeState g_state;
Workspace g_work;

int main(void)
{
  u8 *base = g_work.motion.scratch.layout.scratch;
  ScratchReset();
  assert(ScratchAlloc(16) == base);
  assert(ScratchAlloc(384) == base + 16);
  ScratchReset();
  assert(ScratchAlloc(0x400) == base);
  return 0;
}
