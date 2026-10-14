#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/* A bounded weak-bias check, before the receiver can drive GPIO5 high. */
int StickDiodePreflight(void);
void StickDiodeBegin(void);
void StickDiodeReceivedPacket(void);
void StickDiodeEnd(unsigned connection_error);
void StickDiodeService(void);
uint64_t StickDiodeNextDeadline(void);
void StickDiodeDraw(void);
void StickDiodeInvalidate(void);
#ifdef PW_STICK_BENCH_CONTROL
unsigned StickDiodeProbe(void);
unsigned StickDiodeProbePin(unsigned pin);
#endif
#ifdef __cplusplus
}
#endif
