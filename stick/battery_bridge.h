#ifndef PW_STICK_BATTERY_BRIDGE_H
#define PW_STICK_BATTERY_BRIDGE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

u16 StickBatteryMillivolts(void);
int StickBatteryPercentEstimate(u16 millivolts);
u8 StickBatteryLow(void);

#ifdef __cplusplus
}
#endif

#endif
