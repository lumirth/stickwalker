#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { FRONT = 1, SIDE = 2, POWER = 4, ALL_BUTTONS = 7 };

typedef struct {
    uint8_t held;
    uint8_t pressed;
    uint8_t released;
} Switches;

typedef struct {
    unsigned millivolts;
    bool external, known, low, depleted;
} Battery;

void board_init(void);
Switches board_switches(bool screen_asleep);
Battery board_battery(void);
void board_power_tick(bool linking, bool screen_asleep);
bool board_link_power_ready(void);
bool board_lcd_power(bool on);
bool board_audio_ready(void);
bool board_amp_power(bool on);
bool board_ir_power(bool on);
bool board_codec_write(uint8_t reg, uint8_t value);
bool board_codec_read(uint8_t reg, uint8_t *value);
typedef enum { MOTION_OFF, MOTION_LIVE, MOTION_FIFO, MOTION_IDLE } MotionMode;
void board_motion(MotionMode mode);
void board_motion_drain(void);
uint64_t board_motion_until(void);
uint64_t board_motion_deadline(uint64_t sample_us);
bool board_acceleration(uint64_t sample_us, int8_t out[3]);
void board_wait(uint64_t deadline, bool screen_asleep);
void board_shutdown(void);
void board_park(uint64_t pins);
