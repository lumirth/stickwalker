extern "C" {
#include "application/pw_accel_bma150.h"
}
#include "motion_sample.h"

#include <string.h>

namespace {
u8 range_bandwidth = ACCEL_RANGE_2G_BANDWIDTH_1500HZ;
u8 control = ACCEL_CONTROL_AWAKE;
}

extern "C" u8 AccelInit(void) {
  range_bandwidth = ACCEL_RANGE_2G_BANDWIDTH_1500HZ;
  control = ACCEL_CONTROL_AWAKE;
  return 1;
}

extern "C" u8 AccelRead(u8 address, u8 *destination, u8 count) {
  if (!destination) return 0;
  memset(destination, 0, count);
  if (address == ACCEL_REG_X_LSB && count >= 6) {
    s8 x, y, z;
    StickMotionSample(&x, &y, &z);
    destination[1] = u8(x);
    destination[3] = u8(y);
    destination[5] = u8(z);
  } else if (address == ACCEL_REG_RANGE_BANDWIDTH) {
    destination[0] = range_bandwidth;
  } else if (address == ACCEL_REG_CONTROL) {
    destination[0] = control;
  } else if (address == 0x00) {
    destination[0] = 2;  // BMA150 identity expected by the original driver.
  }
  return 0;
}

extern "C" void AccelWrite(u8 address, u8 value) {
  if (address == ACCEL_REG_RANGE_BANDWIDTH) range_bandwidth = value;
  if (address == ACCEL_REG_CONTROL) control = value;
}
