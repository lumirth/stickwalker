#include "input_bridge.h"

#include "controls.h"

#include <Arduino.h>
#include <M5Unified.h>

namespace {

pw_stick::Controls controls;
bool power_button_ready = false;

bool pm1_read(uint8_t reg, uint8_t &value) {
  return m5::In_I2C.readRegister(0x6e, reg, &value, 1, 100000);
}

bool pm1_write(uint8_t reg, uint8_t value) {
  return m5::In_I2C.writeRegister(0x6e, reg, &value, 1, 100000);
}

bool enable_power_button_input() {
  // Preserve the long-hold recovery path. Change only the short-reset and
  // double-off bits, then read both registers back before using L as input.
  uint8_t single, twice, single_after, twice_after;
  if (!pm1_read(0x49, single) || !pm1_read(0x4a, twice)) return false;
  if (!pm1_write(0x49, uint8_t(single | 1u)) ||
      !pm1_write(0x4a, uint8_t(twice | 1u)) ||
      !pm1_read(0x49, single_after) || !pm1_read(0x4a, twice_after))
    return false;
  return (single_after & 1u) && (twice_after & 1u);
}

}  // namespace

extern "C" void StickInputInit(void) {
  pinMode(11, INPUT_PULLUP);
  pinMode(12, INPUT_PULLUP);
  power_button_ready = enable_power_button_input();
  controls.configure(pw_stick::Profile::Comfort,
                     pw_stick::Orientation::LeftSideDown);
}

extern "C" void StickInputPoll(unsigned long milliseconds) {
  uint8_t power_state = 0;
  const bool power_pressed = power_button_ready &&
      pm1_read(0x48, power_state) && (power_state & 1u);
  controls.sample(uint32_t(milliseconds), digitalRead(11) == LOW,
                  digitalRead(12) == LOW, power_pressed);
}

extern "C" u8 StickInputLevels(void) { return controls.next_scan(); }

extern "C" int StickMenuRequested(void) {
  return controls.menu_requested() ? 1 : 0;
}
