#ifndef STICKWALKER_POKEWALKER_H
#define STICKWALKER_POKEWALKER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  PW_SCREEN_WIDTH = 96,
  PW_SCREEN_HEIGHT = 64,
  PW_SCREEN_PIXELS = PW_SCREEN_WIDTH * PW_SCREEN_HEIGHT
};

typedef enum {
  PW_MOTION_ACTIVE,
  PW_MOTION_IDLE,
  PW_MOTION_PAUSED
} PwMotionMode;

void pw_init(void);
uint64_t pw_tick(void);
bool pw_save(void);
bool pw_save_changes(void);
bool pw_communicating(void);
bool pw_connection_screen(void);
bool pw_sound_enabled(void);
bool pw_sound_playing(void);
void pw_wake_screen(void);
void pw_keep_awake(void);
void pw_sleep_screen(void);
void pw_cancel_connection(void);
void pw_elapsed(uint32_t seconds);
PwMotionMode pw_motion_mode(void);
bool pw_clock_time(uint8_t *hour, uint8_t *minute);

#ifdef __cplusplus
}
#endif
#endif
