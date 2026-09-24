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
void StickSoundService(void);

#ifdef __cplusplus
}
#endif

#endif
