#pragma once

#include <stdint.h>

namespace pw_stick {
// Normalize the six-bit green channel to the five-bit red and blue channels.
// The resulting eight-bit values differ by at most one level.
constexpr uint16_t neutral_gray(unsigned red_blue) {
  const unsigned green = (red_blue * 63 + 15) / 31;
  return uint16_t((red_blue << 11) | (green << 5) | red_blue);
}
constexpr uint16_t kDarkPalette[4] = {
    neutral_gray(0), neutral_gray(10), neutral_gray(21), neutral_gray(31)};
constexpr uint16_t palette_color(bool dark, unsigned shade) {
  return kDarkPalette[dark ? (shade & 3u) : (3u - (shade & 3u))];
}
}  // namespace pw_stick
