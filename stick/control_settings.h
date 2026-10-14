#pragma once

#include "controls.h"

namespace pw_stick {
enum class Orientation : uint8_t { RightSideDown = 0, LeftSideDown = 1 };
constexpr bool right_side_down(unsigned setting) {
  return setting == unsigned(Orientation::RightSideDown);
}
constexpr unsigned panel_rotation(unsigned setting) {
  return right_side_down(setting) ? 3 : 1;
}
struct ControlSettings {
  Layout layout = Layout::ThreeKeyLR;
  uint8_t orientation = uint8_t(Orientation::LeftSideDown);
  uint8_t chord_window = 0;
};

constexpr uint8_t encode_controls(ControlSettings value) {
  return uint8_t(value.layout) | value.orientation << 2 | value.chord_window << 3;
}

constexpr ControlSettings decode_controls(uint8_t value) {
  if ((value & 0xe0u) || ((value >> 3) & 3u) > 2) return {};
  return {Layout(value & 3u), uint8_t((value >> 2) & 1u),
          uint8_t((value >> 3) & 3u)};
}

}  // namespace pw_stick
