#include "battery_bridge.h"

#include <M5Unified.h>

extern "C" u16 StickBatteryMillivolts(void) {
  const int voltage = M5.Power.getBatteryVoltage();
  return voltage > 0 ? u16(voltage) : 0;
}

extern "C" u8 StickBatteryLow(void) {
  const int level = M5.Power.getBatteryLevel();
  return level >= 0 && level < 5;
}
