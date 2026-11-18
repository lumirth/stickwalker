#pragma once
#include <stdint.h>
#include <stdbool.h>

uint64_t audio_tone(uint32_t millihertz, uint8_t volume);
void audio_prepare(void);
uint64_t audio_tick(bool score_active);
void audio_off(void);
bool audio_active(void);
