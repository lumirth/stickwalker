#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void StickNativeInit(void);
void StickNativeStart(void);
void StickNativeService(void);
uint64_t StickNativeNextDeadline(void);
int StickNativeRun(void);
void StickNativeSetCcr(uint8_t value);
uint8_t StickNativeGetCcr(void);
void StickNativeCenterInterrupt(void);
void StickNativeCancelCenterInterrupt(void);
void StickNativeMotionClock(int enabled);
void StickNativeRefreshInterrupt(int enabled);
void StickNativeTimerWEnable(void);
void StickNativeTimerWDisable(void);
void StickNativeTimerWSetCompare(uint16_t compare);
void StickNativeTimerWInterruptEnable(int enabled);
void StickNativeRtcSet(uint32_t seconds);
void StickNativeRtcRead(uint8_t *second, uint8_t *minute, uint8_t *hour);

#ifdef __cplusplus
}
#endif
