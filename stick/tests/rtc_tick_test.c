#include "application/pw_rtc.h"
#include "project.h"

#include <assert.h>

RuntimeState g_state;
void RtcQuarterSecondInterrupt(void);
void RtcSecondInterrupt(void);

int main(void)
{
  g_state.save.rtcSeconds = 86399;
  g_state.idleSeconds[IDLE_DISPLAY] = 2;
  g_state.idleSeconds[IDLE_MOTION] = 1;
  RtcSetTime(g_state.save.rtcSeconds);
  assert(g_state.time.hourBcd24h == 0x23);
  assert(g_state.time.minuteBcd == 0x59);
  assert(g_state.time.secondBcd == 0x59);
  RtcQuarterSecondInterrupt();
  assert(g_state.events.byte & EVENT_UI_REFRESH);
  RtcSecondInterrupt();
  assert(g_state.save.rtcSeconds == 86400);
  assert(g_state.time.hourBcd24h == 0);
  assert(g_state.time.minuteBcd == 0);
  assert(g_state.time.secondBcd == 0);
  assert((g_state.time.pendingUpdates & 3) == 3);
  assert(g_state.idleSeconds[IDLE_DISPLAY] == 1);
  assert(g_state.idleSeconds[IDLE_MOTION] == 0);
  return 0;
}
