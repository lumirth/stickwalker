#ifndef PW_STICK_INPUT_BRIDGE_H
#define PW_STICK_INPUT_BRIDGE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void StickInputInit(void);
/* Poll physical levels frequently enough to queue taps and resolve chords. */
void StickInputPoll(unsigned long milliseconds);
/* Consume one native Pokewalker input scan, returning BUTTON_* bits. */
u8 StickInputLevels(void);
int StickMenuRequested(void);

#ifdef __cplusplus
}
#endif

#endif
