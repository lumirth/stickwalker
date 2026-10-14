#pragma once

#include <stdint.h>

namespace pw_stick {
enum class DiodeState : uint8_t {
  Hidden,
  Missing,
  Reversed,
  Connected,
  Unknown
};

constexpr uint16_t kDiodeIcons[4][16] = {
    {0x0000, 0x0078, 0x0084, 0x0102, 0x0102, 0x0102, 0x0102, 0x01fe, 0x0048,
     0x0048, 0x4448, 0x2848, 0x1048, 0x2848, 0x4400, 0x0000},
    {0x0000, 0x0078, 0x00fc, 0x01f6, 0x01f6, 0x01fe, 0x01fe, 0x01fe, 0x0048,
     0x4048, 0xfc48, 0x4048, 0x0048, 0x0848, 0xfc00, 0x0800},
    {0x0000, 0x0078, 0x00fc, 0x01f6, 0x01f6, 0x01fe, 0x01fe, 0x01fe, 0x0048,
     0x0048, 0x0048, 0x0048, 0x0048, 0x0048, 0x0000, 0x0000},
    {0x0000, 0x0078, 0x0084, 0x0102, 0x0102, 0x0102, 0x0102, 0x01fe, 0x0048,
     0x3848, 0x4448, 0x4048, 0x2048, 0x1048, 0x0000, 0x1000},
};

struct DiodeFeedback {
  DiodeState state = DiodeState::Hidden;
  uint64_t expires = UINT64_MAX;
  void show(DiodeState value, uint64_t now)
  {
    state = value;
    expires =
        value == DiodeState::Hidden
            ? UINT64_MAX
            : now + (value == DiodeState::Connected ? 2000000u : 6000000u);
  }
  bool expire(uint64_t now)
  {
    if (now < expires || state == DiodeState::Hidden)
      return false;
    show(DiodeState::Hidden, now);
    return true;
  }
};

// The icon stays at the Hat end and remains upright after a 180-degree flip.
constexpr unsigned diode_icon_x(unsigned right_side_down)
{
  return right_side_down ? 220 : 4;
}
constexpr unsigned kDiodeIconY = 59;

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
