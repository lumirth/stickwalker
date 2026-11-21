#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum { SOUND_PIEZO, SOUND_PIEZO_LOUD, SOUND_SPEAKER, SOUND_MODES } SoundMode;

uint64_t audio_tone(uint32_t millihertz, uint8_t volume);
void audio_mode(SoundMode mode);
void audio_preview(void);
void audio_prepare(void);
uint64_t audio_tick(bool score_active);
void audio_off(void);
bool audio_active(void);
