#ifndef PW_STICK_POWER_SLEEP_H
#define PW_STICK_POWER_SLEEP_H

#include <stdint.h>

// Retained-state ESP32 light sleep. The caller owns the application clock and
// supplies the next deadline; this layer only suspends the CPU between work.
bool StickSleepBegin(void);
bool StickSleepUntil(uint64_t deadline_us);

#ifdef PW_STICK_BENCH_CONTROL
void StickSleepDiagnostic(uint64_t *count, uint64_t *sleep_us,
                         uint64_t *timer_wakes, uint64_t *gpio_wakes,
                         uint64_t *errors, int *last_error,
                         unsigned *last_gpio, unsigned *last_duration);
#endif

#endif
