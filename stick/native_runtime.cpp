#include "native_runtime.h"

#include "foreground_bridge.h"
#include <esp_timer.h>

extern "C" {
void RtcQuarterSecondInterrupt(void);
void RtcSecondInterrupt(void);
void RtcMinuteInterrupt(void);
void RtcHourInterrupt(void);
void BeepAdvance(void);
}

namespace {
constexpr uint64_t kWatchHz = 32768;
constexpr uint64_t kSampleUs = 62500;
bool initialized = false;
bool servicing = false;
bool foreground = false;
uint8_t ccr = 0x80;
bool motion_clock = true;
bool sample_interrupt = false;
bool refresh_interrupt = true;
bool sample_pending = false;
bool refresh_pending = false;
bool second_pending = false;
bool minute_pending = false;
bool hour_pending = false;
bool center_pending = false;
bool wake_pending = false;
uint64_t sample_deadline = 0;
uint64_t sample_remaining = kSampleUs;
uint64_t rtc_anchor = 0;
uint32_t rtc_base = 0;
uint64_t rtc_seconds = 0;
uint64_t rtc_quarters = 0;
bool timer_w_enabled = false;
bool timer_w_interrupt = true;
bool timer_w_pending = false;
bool timer_w_dispatch = false;
uint16_t timer_w_compare = 0;
uint64_t timer_w_deadline = 0;
uint64_t timer_w_event = 0;
unsigned timer_w_revision = 0;

uint64_t now_us() { return uint64_t(esp_timer_get_time()); }
uint64_t timer_period() { return uint64_t(timer_w_compare + 1) * 1000000; }
uint8_t bcd(unsigned value) { return uint8_t((value / 10) * 16 + value % 10); }

void capture(uint64_t now) {
  const uint64_t elapsed = now - rtc_anchor;
  const uint64_t seconds = elapsed / 1000000;
  const uint64_t quarters = elapsed / 250000;
  if (seconds != rtc_seconds) {
    const uint64_t before = uint64_t(rtc_base) + rtc_seconds;
    const uint64_t after = uint64_t(rtc_base) + seconds;
    second_pending = true;
    minute_pending |= before / 60 != after / 60;
    hour_pending |= before / 3600 != after / 3600;
    rtc_seconds = seconds;
  }
  if (quarters != rtc_quarters) {
    refresh_pending = true;
    rtc_quarters = quarters;
  }
  if (motion_clock && now >= sample_deadline) {
    sample_pending = true;
    sample_deadline += ((now - sample_deadline) / kSampleUs + 1) * kSampleUs;
  }
}
}

extern "C" void StickNativeInit(void) {
  initialized = true;
  servicing = foreground = false;
  ccr = 0x80;
  motion_clock = refresh_interrupt = true;
  sample_interrupt = false;
  sample_pending = refresh_pending = second_pending = minute_pending = false;
  hour_pending = center_pending = wake_pending = false;
  rtc_anchor = now_us();
  rtc_base = 0;
  rtc_seconds = rtc_quarters = 0;
  sample_deadline = rtc_anchor + kSampleUs;
  sample_remaining = kSampleUs;
  timer_w_enabled = timer_w_pending = timer_w_dispatch = false;
  timer_w_interrupt = true;
  timer_w_compare = 0;
  timer_w_deadline = timer_w_event = 0;
  timer_w_revision = 0;
}

extern "C" void StickNativeStart(void) {
  sample_deadline = now_us() + kSampleUs;
  sample_remaining = kSampleUs;
  sample_pending = false;
  motion_clock = true;
  sample_interrupt = true;
  ccr = 0;
  StickNativeService();
}

extern "C" void StickNativeService(void) {
  if (!initialized || servicing) return;
  servicing = true;
  const uint64_t now = now_us();
  const uint64_t watch_now = now * kWatchHz;
  capture(now);
  if (ccr & 0x80) {
    if (timer_w_enabled && watch_now >= timer_w_deadline) {
      timer_w_pending = true;
      timer_w_deadline += ((watch_now - timer_w_deadline) / timer_period() + 1) * timer_period();
    }
    servicing = false;
    return;
  }
  if (center_pending) {
    center_pending = false;
    StickForegroundAcceptCenterInterrupt();
    StickNativeMotionClock(1);
    wake_pending |= !foreground;
  }
  if (refresh_pending && refresh_interrupt) {
    refresh_pending = false;
    RtcQuarterSecondInterrupt();
    wake_pending |= !foreground;
  }
  if (second_pending) {
    second_pending = false;
    RtcSecondInterrupt();
    wake_pending |= !foreground;
  }
  if (minute_pending) {
    minute_pending = false;
    RtcMinuteInterrupt();
    wake_pending |= !foreground;
  }
  if (hour_pending) {
    hour_pending = false;
    RtcHourInterrupt();
    wake_pending |= !foreground;
  }
  if (sample_pending && sample_interrupt) {
    sample_pending = false;
    wake_pending |= !foreground;
  }
  if (timer_w_enabled && !timer_w_interrupt && watch_now >= timer_w_deadline) {
    timer_w_pending = true;
    timer_w_deadline += ((watch_now - timer_w_deadline) / timer_period() + 1) * timer_period();
  }
  if (timer_w_enabled && timer_w_interrupt && timer_w_pending) {
    timer_w_pending = false;
    timer_w_event = watch_now;
    timer_w_dispatch = true;
    BeepAdvance();
    timer_w_dispatch = false;
    wake_pending |= !foreground;
  }
  while (timer_w_enabled && timer_w_interrupt && timer_w_deadline <= watch_now) {
    timer_w_event = timer_w_deadline;
    const unsigned revision = timer_w_revision;
    timer_w_dispatch = true;
    BeepAdvance();
    timer_w_dispatch = false;
    if (revision == timer_w_revision) timer_w_deadline += timer_period();
    wake_pending |= !foreground;
  }
  servicing = false;
}

extern "C" uint64_t StickNativeNextDeadline(void) {
  if (!initialized) return UINT64_MAX;
  if (!(ccr & 0x80) && (wake_pending ||
      (StickForegroundIsBeep() && timer_w_compare == 0))) return now_us();
  uint64_t next = rtc_anchor + (rtc_seconds + 1) * 1000000;
  if (refresh_interrupt) {
    const uint64_t quarter = rtc_anchor + (rtc_quarters + 1) * 250000;
    if (quarter < next) next = quarter;
  }
  if (motion_clock && sample_interrupt && sample_deadline < next) next = sample_deadline;
  if (timer_w_enabled && timer_w_interrupt) {
    const uint64_t timer = (timer_w_deadline + kWatchHz - 1) / kWatchHz;
    if (timer < next) next = timer;
  }
  return next;
}

extern "C" int StickNativeRun(void) {
  StickNativeService();
  if (foreground) return 0;
  const int task = StickForegroundIsIr() ? 3 : StickForegroundIsBeep() ? 2 : 1;
  if (task != 3 && ((ccr & 0x80) || (!wake_pending && !(task == 2 && timer_w_compare == 0))))
    return 0;
  wake_pending = false;
  sample_interrupt = false;
  foreground = true;
  StickForegroundRun();
  sample_interrupt = StickForegroundIsMain();
  StickNativeService();
  foreground = false;
  return task;
}

extern "C" void StickNativeSetCcr(uint8_t value) {
  StickNativeService();
  ccr = value;
  StickNativeService();
}

extern "C" uint8_t StickNativeGetCcr(void) { return ccr; }

extern "C" void StickNativeCenterInterrupt(void) {
  center_pending = true;
}

extern "C" void StickNativeCancelCenterInterrupt(void) { center_pending = false; }

extern "C" void StickNativeMotionClock(int enabled) {
  if (motion_clock == bool(enabled)) return;
  const uint64_t now = now_us();
  capture(now);
  if (enabled) sample_deadline = now + sample_remaining;
  else sample_remaining = sample_deadline > now ? sample_deadline - now : kSampleUs;
  motion_clock = enabled;
}

extern "C" void StickNativeRefreshInterrupt(int enabled) {
  capture(now_us());
  refresh_interrupt = enabled;
}

extern "C" void StickNativeTimerWEnable(void) {
  timer_w_enabled = timer_w_interrupt = true;
  timer_w_pending = false;
  timer_w_deadline = now_us() * kWatchHz + timer_period();
  ++timer_w_revision;
}

extern "C" void StickNativeTimerWDisable(void) {
  timer_w_enabled = timer_w_pending = false;
  ++timer_w_revision;
}

extern "C" void StickNativeTimerWSetCompare(uint16_t compare) {
  timer_w_compare = compare;
  timer_w_deadline = (timer_w_dispatch ? timer_w_event : now_us() * kWatchHz) + timer_period();
  ++timer_w_revision;
}

extern "C" void StickNativeTimerWInterruptEnable(int enabled) {
  StickNativeService();
  timer_w_interrupt = enabled;
  StickNativeService();
}

extern "C" void StickNativeRtcSet(uint32_t seconds) {
  rtc_base = seconds;
  rtc_anchor = now_us();
  rtc_seconds = rtc_quarters = 0;
  refresh_interrupt = true;
  refresh_pending = second_pending = minute_pending = hour_pending = false;
}

extern "C" void StickNativeRtcRead(uint8_t *second, uint8_t *minute, uint8_t *hour) {
  const uint64_t seconds = uint64_t(rtc_base) + (now_us() - rtc_anchor) / 1000000;
  *second = bcd(seconds % 60);
  *minute = bcd((seconds / 60) % 60);
  *hour = bcd((seconds / 3600) % 24);
}
