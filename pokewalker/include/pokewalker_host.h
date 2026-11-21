#ifndef STICKWALKER_POKEWALKER_HOST_H
#define STICKWALKER_POKEWALKER_HOST_H

#include "pokewalker.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif


#pragma pack(push, 8)
typedef struct {
  uint64_t calendar_us;
  uint32_t daily_steps;
  uint32_t hour_steps;
  uint32_t social_elapsed_seconds;
  uint32_t random_state;
  uint8_t save[24];
  uint8_t rollover_hour_bcd;
  uint8_t present;
  uint8_t reserved[6];
} PwRuntimeState;

enum {
  PW_INFRARED_MAX_PACKET = 136,
};

uint64_t pw_host_now_us(void);
/* RTC-backed elapsed calendar time; scheduling uses pw_host_now_us(). */
uint64_t pw_host_calendar_us(void);
/* Latest timestamp available for chronological replay; live hosts return now. */
uint64_t pw_host_acceleration_until(void);
/* Signed acceleration at 64 counts/g, at a monotonic game-sample timestamp. */
bool pw_host_acceleration(uint64_t sample_us, int8_t xyz[3]);
enum { CENTER = 2, LEFT = 4, RIGHT = 8 };
/* Current levels and brief presses use CENTER, LEFT and RIGHT. */
uint8_t pw_host_buttons(void);
void pw_host_present(const uint8_t pixels[PW_SCREEN_PIXELS]);
void pw_host_display_sleep(bool asleep);
void pw_host_contrast(uint8_t setting);
/* The returned monotonic time anchors a cold first note after silent startup.
 * UINT64_MAX reports an unavailable output. */
uint64_t pw_host_tone(uint32_t frequency_millihz, uint8_t volume);
bool pw_host_battery_low(void);

void pw_host_eeprom_read(uint16_t address, void *out, size_t length);
bool pw_host_eeprom_write(uint16_t address, const void *bytes, size_t length);
bool pw_host_commit(void);
bool pw_host_communication(bool active);
/* How the last connection ended, kept for diagnosis over USB. */
void pw_host_link_report(uint8_t result, uint8_t phase, uint8_t last_command, uint16_t packets, uint16_t damaged);

bool pw_host_infrared_open(void);
void pw_host_infrared_detected(void);
void pw_host_infrared_close(void);
typedef struct {
  uint64_t time_us;
  uint8_t value;
  uint8_t errors;
  bool byte;
} PwInfraredEvent;
/* Ordered decoded bytes and the latest observed quiet horizon. */
bool pw_host_infrared_receive(PwInfraredEvent *event);
/* Wire completion time; UINT64_MAX on failure. */
uint64_t pw_host_infrared_transmit(const uint8_t *bytes, size_t length);

/* The applied calendar timestamp is the last whole second reflected in state. */
bool pw_host_runtime_load(PwRuntimeState *state);
void pw_host_runtime_checkpoint(const PwRuntimeState *state);
void pw_host_device_id(uint8_t out[40]);
void pw_host_fault(const char *message);

#pragma pack(pop)

#ifdef __cplusplus
}
static_assert(sizeof(PwRuntimeState) == 56, "Runtime checkpoint layout");
#else
_Static_assert(sizeof(PwRuntimeState) == 56, "Runtime checkpoint layout");
#endif
#endif
