#pragma once

#include "controls.h"

namespace pw_stick {
struct ControlSettings {
  Layout layout = Layout::TwoKeyMR;
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

// Preserve the user's actual directions when migrating the old coupled layout.
constexpr ControlSettings migrate_controls(uint8_t value) {
  const bool three = value & 1u;
  const uint8_t orientation = (value >> 1) & 1u;
  const uint8_t chord = (value >> 2) & 3u;
  return {three ? (orientation ? Layout::ThreeKeyLR : Layout::ThreeKeyRL)
                : (orientation ? Layout::TwoKeyRM : Layout::TwoKeyMR),
          orientation, uint8_t(chord > 2 ? 0 : chord)};
}
}  // namespace pw_stick
