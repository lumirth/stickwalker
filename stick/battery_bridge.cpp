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
