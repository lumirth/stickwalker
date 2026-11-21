#pragma once
#include <stdint.h>

// What the firmware was doing, kept across a reset in RTC memory.
typedef enum {
    ACTIVITY_BOOT = 1,
    ACTIVITY_GAME,
    ACTIVITY_LINK,
    ACTIVITY_SAVE,
    ACTIVITY_USB,
    ACTIVITY_SLEEP,
} Activity;

void diagnostics_init(void);
void diagnostics_mark(Activity activity);
// Records how the last connection ended, for a host to read later.
void diagnostics_link(uint8_t result, uint8_t phase, uint8_t last_command, uint16_t packets, uint16_t damaged);
// Appends the diagnostics record to a status reply; returns its length.
unsigned diagnostics_status(uint8_t *out);
