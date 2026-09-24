#ifndef PW_STICK_MOTION_SAMPLE_H
#define PW_STICK_MOTION_SAMPLE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Equivalent signed high bytes from a BMA150 in ±2 g mode, at the native
 * firmware's sample cadence. M5Unified already maps BMI270 chip axes to the
 * Stick board frame. */
void StickMotionSample(s8 *x, s8 *y, s8 *z);

#ifdef __cplusplus
}
#endif

#endif
