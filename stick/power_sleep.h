#ifndef PW_STICK_POWER_SLEEP_H
#define PW_STICK_POWER_SLEEP_H

#include <stdint.h>

// Suspend the CPU in Light-sleep until the caller's next work deadline.
bool StickSleepBegin(void);
bool StickSleepUntil(uint64_t deadline_us);
uint64_t StickSleepNextDeadline(void);

#ifdef PW_STICK_BENCH_CONTROL
void StickSleepDiagnostic(uint64_t *count, uint64_t *sleep_us,
                         uint64_t *timer_wakes, uint64_t *gpio_wakes,
                         uint64_t *errors, uint64_t *rejections, int *last_error,
                         unsigned *last_gpio, unsigned *last_duration);
#endif

#endif
