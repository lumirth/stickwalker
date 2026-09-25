#include "battery_bridge.h"

#include "board_hal.h"

extern "C" u16 StickBatteryMillivolts(void) {
  // Board startup initializes StickBoardPower() directly. M5.Power is not
  // initialized because M5.begin() changes the GPIO5 optical detector.
  return StickBoardPower().getBatteryVoltage();
}

extern "C" u8 StickBatteryLow(void) {
  const u16 voltage = StickBatteryMillivolts();
  // Match M5Unified's existing <5% voltage indication, without calling its
  // uninitialized Power_Class. A failed I2C read is unknown, not low.
  return voltage != 0 && voltage < 3340;
}
