#include "sound_bridge.h"

#include <M5Unified.h>
#include <esp_timer.h>

extern "C" void BeepAdvance(void);

namespace {
bool enabled = false;
u16 compare_value = 0;
u8 output_mode = 0;
int64_t next_period_us = 0;

int64_t period_us() {
  return (int64_t(compare_value + 1) * 1000000 + 32767) / 32768;
}
}  // namespace

extern "C" void StickSoundInit(void) {
  M5.Speaker.begin();
  enabled = false;
  compare_value = 0;
  output_mode = 0;
  M5.Speaker.stop();
}

extern "C" void StickSoundEnable(void) {
  enabled = true;
  next_period_us = esp_timer_get_time();
}

extern "C" void StickSoundDisable(void) {
  enabled = false;
  M5.Speaker.stop();
}

extern "C" void StickSoundPeriod(u16 compare, u8 mode) {
  compare_value = compare;
  output_mode = mode;
  if (!enabled || !compare || !output_mode) {
    M5.Speaker.stop();
    return;
  }
  M5.Speaker.tone(32768.0f / float(compare + 1));
}

extern "C" void StickSoundSilencePeriod(u16 compare) {
  compare_value = compare;
  M5.Speaker.stop();
}

extern "C" void StickSoundMute(void) { M5.Speaker.stop(); }

extern "C" void StickSoundService(void) {
  if (!enabled) return;
  const int64_t now = esp_timer_get_time();
  unsigned serviced = 0;
  while (next_period_us <= now && serviced < 256) {
    BeepAdvance();
    next_period_us += period_us();
    ++serviced;
  }
  if (serviced == 256 && next_period_us <= now)
    next_period_us = now + period_us();
}
