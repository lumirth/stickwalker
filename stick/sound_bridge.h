#ifndef PW_STICK_SOUND_BRIDGE_H
#define PW_STICK_SOUND_BRIDGE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void StickSoundInit(void);
void StickSoundEnable(void);
void StickSoundDisable(void);
void StickSoundPeriod(u16 compare, u8 outputMode);
void StickSoundSilencePeriod(u16 compare);
void StickSoundMute(void);
void StickSoundQuiesceForIr(void);
void StickSoundService(void);
int StickSoundTestTone(void);
#ifdef PW_STICK_BENCH_CONTROL
int StickSoundBenchTone(void);
int StickSoundBenchToneActive(void);
void StickSoundDiagnostic(unsigned *enabled, unsigned *ready,
                          unsigned *codec, unsigned *mode,
                          unsigned *compare, unsigned *begins,
                          unsigned *begin_failures, unsigned *power_failures,
                          unsigned *tones, unsigned *tone_failures,
                          unsigned *playing);
void StickSoundTimingDiagnostic(unsigned *measured, unsigned *under_20ms,
                               unsigned *under_50ms, unsigned *shortest_us);
#endif

#ifdef __cplusplus
}
#endif

#endif
