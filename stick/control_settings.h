#pragma once

#include "controls.h"

namespace pw_stick {
struct ControlSettings {
  Layout layout = Layout::ThreeKeyLR;
  uint8_t orientation = 0;
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
