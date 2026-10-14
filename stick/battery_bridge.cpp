#include "battery_bridge.h"

#include "board_hal.h"

extern "C" u16 StickBatteryMillivolts(void) {
  // Board startup initializes this PMIC driver directly.
  return StickBoardPower().getBatteryVoltage();
}

extern "C" u8 StickBatteryLow(void) {
  const u16 voltage = StickBatteryMillivolts();
  // Use M5Unified's <5% threshold. Zero indicates a failed voltage read.
  return voltage != 0 && voltage < 3340;
}

extern "C" int StickBatteryPercentEstimate(u16 millivolts) {
  if (!millivolts) return -1;
  // M5Unified uses this voltage-only scale for M5PM1. It is an estimate,
  // particularly while charging or under a changing load.
  if (millivolts <= 3300) return 0;
  if (millivolts >= 4100) return 100;
  return (millivolts - 3300) * 100 / 800;
}
