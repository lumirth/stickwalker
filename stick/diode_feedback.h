#pragma once

#include <stdint.h>
#include "control_settings.h"

namespace pw_stick {
enum class DiodeState : uint8_t {
  Hidden,
  Missing,
  Reversed,
  Connected,
  Unknown
};

constexpr uint16_t kDiodeIcons[3][16] = {
    {0x0000, 0x0078, 0x0084, 0x0102, 0x0102, 0x0102, 0x0102, 0x01fe, 0x0048,
     0x0048, 0x4448, 0x2848, 0x1048, 0x2848, 0x4400, 0x0000},
    {0x0000, 0x0078, 0x00fc, 0x01f6, 0x01f6, 0x01fe, 0x01fe, 0x01fe, 0x0048,
     0x4048, 0xfc48, 0x4048, 0x0048, 0x0848, 0xfc00, 0x0800},
    {0x0000, 0x0078, 0x00fc, 0x01f6, 0x01f6, 0x01fe, 0x01fe, 0x01fe, 0x0048,
     0x0048, 0x0048, 0x0048, 0x0048, 0x0048, 0x0000, 0x0000},
};

struct DiodeFeedback {
  DiodeState measurement = DiodeState::Unknown;
  bool transferring = false;

  void begin() { measurement = DiodeState::Unknown; transferring = true; }
  void observe(DiodeState value) { measurement = value; }
  void end() { transferring = false; }
  void clear() { measurement = DiodeState::Unknown; transferring = false; }
  DiodeState visible(bool result_open) const
  {
    if (!transferring && !result_open) return DiodeState::Hidden;
    if (measurement == DiodeState::Missing || measurement == DiodeState::Reversed)
      return measurement;
    return transferring && measurement == DiodeState::Connected
        ? DiodeState::Connected : DiodeState::Hidden;
  }
};

// Keep the connector corner and four pixels of padding after rotation.
constexpr unsigned diode_icon_x(unsigned setting)
{
  return right_side_down(setting) ? 220 : 4;
}
constexpr unsigned diode_icon_y(unsigned setting)
{
  return right_side_down(setting) ? 115 : 4;
}

// With a weak pull-down, positive photocurrent into GPIO5 identifies an
// illuminated reversed diode. Correct polarity sinks photocurrent instead.
// Darkness and an empty socket remain indistinguishable here.
inline DiodeState diode_voltage_state(const int (&millivolts)[3])
{
  int low = 800, high = 200;
  for (int value : millivolts) {
    if (value < 200 || value > 800)
      return DiodeState::Unknown;
    if (value < low)
      low = value;
    if (value > high)
      high = value;
  }
  return high - low <= 50 ? DiodeState::Reversed : DiodeState::Unknown;
}

} // namespace pw_stick
