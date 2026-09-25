#include "power_sleep.h"

#include <Arduino.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_timer.h>

namespace {
bool ready = false;
uint64_t sleep_count = 0;
uint64_t total_sleep_us = 0;
uint64_t timer_wakes = 0;
uint64_t gpio_wakes = 0;
uint64_t errors = 0;
int last_error = 0;
unsigned last_gpio = 0;
unsigned last_duration = 0;

}  // namespace

bool StickSleepBegin(void) {
  const uint64_t wake_pins = (1ULL << 11) | (1ULL << 12);
  // The front buttons are RTC capable. Keep their pull-ups supplied during
  // light sleep so a released switch has a defined HIGH level at the RTC
  // wake controller as well as at digitalRead(). The side PM1 button retains
  // its press event; a 100 ms timer poll handles that input without changing
  // the PM1 IRQ routing used by the established receiver build.
  for (auto pin : {GPIO_NUM_11, GPIO_NUM_12}) {
    if (rtc_gpio_pullup_en(pin) != ESP_OK ||
        rtc_gpio_pulldown_dis(pin) != ESP_OK) {
      ++errors;
      return false;
    }
  }
  if (esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON) != ESP_OK ||
      esp_sleep_enable_ext1_wakeup_io(wake_pins,
                                     ESP_EXT1_WAKEUP_ANY_LOW) != ESP_OK) {
    ++errors;
    return false;
  }
  ready = true;
  return true;
}

bool StickSleepUntil(uint64_t deadline_us) {
  if (!ready) return false;
  const uint64_t now = uint64_t(esp_timer_get_time());
  if (deadline_us <= now + 3000) return false;
  // The side PM1 button is read from its latched register every 100 ms at
  // most. Front buttons wake directly, without waiting for that timer.
  uint64_t duration = deadline_us - now - 1000;
  if (duration > 100000) duration = 100000;
  if (digitalRead(11) == LOW || digitalRead(12) == LOW) return false;
  if (esp_sleep_enable_timer_wakeup(duration) != ESP_OK) {
    ++errors;
    last_error = -1;
    return false;
  }
  const esp_err_t result = esp_light_sleep_start();
  const uint64_t elapsed = uint64_t(esp_timer_get_time()) - now;
  if (result != ESP_OK) {
    ++errors;
    last_error = result;
    last_gpio = (digitalRead(11) == LOW ? 1u : 0u) |
                (digitalRead(12) == LOW ? 2u : 0u);
    last_duration = duration;
    return false;
  }
  ++sleep_count;
  total_sleep_us += elapsed;
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
                         uint64_t *failures, int *last,
                         unsigned *gpio_state, unsigned *duration) {
  *count = sleep_count;
  *sleep_us = total_sleep_us;
  *timer = timer_wakes;
  *gpio = gpio_wakes;
  *failures = errors;
  *last = last_error;
  *gpio_state = last_gpio;
  *duration = last_duration;
}
#endif
