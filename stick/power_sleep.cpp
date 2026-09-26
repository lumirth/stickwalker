#include "power_sleep.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_timer.h>

namespace {
bool ready = false;
uint64_t retry_begin_us = 0;
uint64_t sleep_count = 0;
uint64_t total_sleep_us = 0;
uint64_t timer_wakes = 0;
uint64_t gpio_wakes = 0;
uint64_t errors = 0;
uint64_t rejections = 0;
int last_error = 0;
unsigned last_gpio = 0;
unsigned last_duration = 0;
#ifdef PW_STICK_BENCH_CONTROL
// Survives the USB controller's CPU reset; no native save or NVS writes.
RTC_NOINIT_ATTR struct {
  uint32_t magic, phase, gpio, duration;
  uint64_t count, total_us, time_us, failures, rejections;
  int error;
} retained;
constexpr uint32_t kRetainedMagic = 0x5057534f;
#endif

void record_failure(int error) {
  ++errors;
  last_error = error;
#ifdef PW_STICK_BENCH_CONTROL
  retained.failures = errors;
  retained.error = error;
#endif
}

bool restore_button_gpio() {
  // EXT1 sleep preparation selects the RTC mux. Digital reads cannot sample
  // these switches again until every sleep return hands both pads back.
  bool ok = true;
  for (auto pin : {GPIO_NUM_11, GPIO_NUM_12}) {
    for (const esp_err_t error : {rtc_gpio_hold_dis(pin), rtc_gpio_deinit(pin),
         gpio_set_direction(pin, GPIO_MODE_INPUT),
         gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY)}) {
      if (error != ESP_OK) { record_failure(error); ok = false; }
    }
  }
  return ok;
}

}  // namespace

bool StickSleepBegin(void) {
#ifdef PW_STICK_BENCH_CONTROL
  static bool reported = false;
  if (!reported) {
    if (retained.magic == kRetainedMagic)
      Serial.printf("PW_STICK_SLEEP_PREVIOUS phase=%u count=%llu "
                    "sleep_us=%llu time_us=%llu failures=%llu error=%d "
                    "gpio=%u duration=%u rejections=%llu\n",
                    unsigned(retained.phase),
                    (unsigned long long)retained.count,
                    (unsigned long long)retained.total_us,
                    (unsigned long long)retained.time_us,
                    (unsigned long long)retained.failures, retained.error,
                    unsigned(retained.gpio), unsigned(retained.duration),
                    (unsigned long long)retained.rejections);
    retained = {};
    retained.magic = kRetainedMagic;
    reported = true;
  }
#endif
  ready = false;
  retry_begin_us = uint64_t(esp_timer_get_time()) + 1000000;
  if (!restore_button_gpio()) return false;
  const uint64_t wake_pins = (1ULL << 11) | (1ULL << 12);
  // The front buttons are RTC capable. Keep their pull-ups supplied during
  // light sleep so a released switch has a defined HIGH level at the RTC
  // wake controller as well as at digitalRead(). The side PM1 button retains
  // its press event; a 100 ms timer poll handles that input without changing
  // the PM1 IRQ routing used by the established receiver build.
  for (auto pin : {GPIO_NUM_11, GPIO_NUM_12}) {
    const esp_err_t up = rtc_gpio_pullup_en(pin);
    const esp_err_t down = rtc_gpio_pulldown_dis(pin);
    if (up != ESP_OK || down != ESP_OK) {
      record_failure(up != ESP_OK ? up : down);
      return false;
    }
  }
  const esp_err_t domain = esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH,
                                             ESP_PD_OPTION_ON);
  const esp_err_t wake = esp_sleep_enable_ext1_wakeup_io(
      wake_pins, ESP_EXT1_WAKEUP_ANY_LOW);
  if (domain != ESP_OK || wake != ESP_OK) {
    record_failure(domain != ESP_OK ? domain : wake);
    return false;
  }
  ready = true;
  return true;
}

bool StickSleepUntil(uint64_t deadline_us) {
  const uint64_t now = uint64_t(esp_timer_get_time());
  if (!ready && (now < retry_begin_us || !StickSleepBegin())) return false;
  if (deadline_us <= now + 3000) return false;
  // The side PM1 button is read from its latched register every 100 ms at
  // most. Front buttons wake directly, without waiting for that timer.
  uint64_t duration = deadline_us - now - 1000;
  if (duration > 100000) duration = 100000;
  if (digitalRead(11) == LOW || digitalRead(12) == LOW) return false;
  const esp_err_t timer_result = esp_sleep_enable_timer_wakeup(duration);
  if (timer_result != ESP_OK) {
    record_failure(timer_result);
    ready = false;
    retry_begin_us = uint64_t(esp_timer_get_time()) + 1000000;
    return false;
  }
#ifdef PW_STICK_BENCH_CONTROL
  retained.phase = 1;
  retained.time_us = now;
#endif
  const esp_err_t result = esp_light_sleep_start();
  const uint64_t elapsed = uint64_t(esp_timer_get_time()) - now;
  // Restore after successful and cancelled entries: pin preparation may have
  // happened before the sleep controller rejected the opportunity.
  const bool buttons_restored = restore_button_gpio();
#ifdef PW_STICK_BENCH_CONTROL
  retained.phase = 2;
  retained.time_us = uint64_t(esp_timer_get_time());
#endif
  if (!buttons_restored) {
    ready = false;
    retry_begin_us = uint64_t(esp_timer_get_time()) + 1000000;
    return false;
  }
  if (result != ESP_OK) {
    last_error = result;
    last_gpio = (digitalRead(11) == LOW ? 1u : 0u) |
                (digitalRead(12) == LOW ? 2u : 0u);
    last_duration = duration;
#ifdef PW_STICK_BENCH_CONTROL
    retained.error = result;
    retained.gpio = last_gpio;
    retained.duration = duration;
#endif
    // A wake source becoming pending, or entry overhead consuming a short
    // interval, cancels this opportunity. Neither invalidates the wake
    // configuration. Return to scheduled work instead of vetoing sleep for
    // a second after every such race.
    if (result == ESP_ERR_SLEEP_REJECT ||
        result == ESP_ERR_SLEEP_TOO_SHORT_SLEEP_DURATION) {
      ++rejections;
#ifdef PW_STICK_BENCH_CONTROL
      retained.rejections = rejections;
#endif
      return false;
    }
    record_failure(result);
    // Reinstall wake configuration after an unexpected sleep failure rather
    // than retrying a broken configuration every millisecond forever.
    ready = false;
    retry_begin_us = uint64_t(esp_timer_get_time()) + 1000000;
    return false;
  }
  ++sleep_count;
  total_sleep_us += elapsed;
#ifdef PW_STICK_BENCH_CONTROL
  retained.count = sleep_count;
  retained.total_us = total_sleep_us;
#endif
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER: ++timer_wakes; break;
    case ESP_SLEEP_WAKEUP_EXT1: ++gpio_wakes; break;
    default: break;
  }
  return true;
}

#ifdef PW_STICK_BENCH_CONTROL
void StickSleepDiagnostic(uint64_t *count, uint64_t *sleep_us,
                         uint64_t *timer, uint64_t *gpio,
                         uint64_t *failures, uint64_t *rejected, int *last,
                         unsigned *gpio_state, unsigned *duration) {
  *count = sleep_count;
  *sleep_us = total_sleep_us;
  *timer = timer_wakes;
  *gpio = gpio_wakes;
  *failures = errors;
  *rejected = rejections;
  *last = last_error;
  *gpio_state = last_gpio;
  *duration = last_duration;
}
#endif
