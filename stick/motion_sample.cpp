#include "motion_sample.h"

#include "board_hal.h"
#include <M5Unified.h>
#include <math.h>

namespace {

int8_t high_byte(float gravity) {
  // BMA150 ±2 g, 10-bit output: 256 counts/g; retain the signed high byte.
  const float scaled = gravity * 64.0f;
  if (scaled <= -128.0f) return -128;
  if (scaled >= 127.0f) return 127;
  return int8_t(lroundf(scaled));
}

}  // namespace

extern "C" void StickMotionSample(s8 *x, s8 *y, s8 *z) {
  float ax = 0, ay = 0, az = 0;
  if (!x || !y || !z) return;
  if (!StickBoardAccel(&ax, &ay, &az)) return;
  *x = high_byte(ax);
  *y = high_byte(ay);
  *z = high_byte(az);
}
