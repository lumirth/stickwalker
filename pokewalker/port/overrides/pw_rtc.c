#include "port_internal.h"
#include "pokewalker.h"
#include "project.h"
#include "application/pw_rtc.h"

/* Host elapsed time replaces the H8 RTC registers and interrupt counters. */
#define RTC_PENDING_MINUTE 1
#define RTC_PENDING_HOUR 2

static u32 native_day_seconds;
static u8 bcd(u8 n)
{
  return ((n / 10) << 4) | (n % 10);
}

static void clock_shadow(void)
{
  g_state.time.secondBcd = bcd(native_day_seconds % 60);
  g_state.time.minuteBcd = bcd((native_day_seconds / 60) % 60);
  g_state.time.hourBcd24h = bcd((native_day_seconds / 3600) % 24);
}

void pw_port_RtcSetTime(u32 seconds)
{
  native_day_seconds = seconds % 86400;
  clock_shadow();
  pw_clock_set_anchor();
}
extern __typeof__(pw_port_RtcSetTime) RtcSetTime
  __attribute__((alias("pw_port_RtcSetTime")));

void pw_elapsed(uint32_t seconds)
{
  while (seconds) {
    u32 chunk = 60 - native_day_seconds % 60;
    if (chunk > seconds)
      chunk = seconds;
    native_day_seconds = (native_day_seconds + chunk) % 86400;
    g_state.save.rtcSeconds += chunk;
    u32 social = g_state.socialElapsedSeconds + chunk;
    g_state.socialElapsedSeconds = social > 3600 ? 3600 : social;
    for (unsigned i = 0; i < 2; i++)
      g_state.idleSeconds[i] =
          g_state.idleSeconds[i] > chunk ? g_state.idleSeconds[i] - chunk : 0;
    seconds -= chunk;
    clock_shadow();
    if (native_day_seconds % 60 == 0)
      g_state.time.pendingUpdates |= RTC_PENDING_MINUTE;
    if (native_day_seconds % 3600 == 0)
      g_state.time.pendingUpdates |= RTC_PENDING_HOUR;
    RtcDispatch();
  }
}

void pw_port_RtcReadStable(u8 *second, u8 *minute, u8 *hour)
{
  *second = g_state.time.secondBcd;
  *minute = g_state.time.minuteBcd;
  *hour = g_state.time.hourBcd24h;
}

extern __typeof__(pw_port_RtcReadStable) RtcReadStable
  __attribute__((alias("pw_port_RtcReadStable")));
