#pragma once

#include "diode_feedback.h"

namespace pw_stick {
struct DiodeCapWindow {
  uint32_t diode = 0;
  uint32_t reference = 0;
  uint32_t internal_low = 0;
  uint32_t internal_high = 0;
};

inline DiodeState diode_cap_state(const DiodeCapWindow (&windows)[2])
{
  int64_t added[2];
  for (unsigned i = 0; i < 2; ++i) {
    const auto &w = windows[i];
    if (w.internal_high <= w.internal_low + 128 || !w.reference ||
        !w.diode || w.diode > 1000000 || w.reference > 1000000)
      return DiodeState::Unknown;
    const int64_t span = w.internal_high - w.internal_low;
    // Two on-chip capacitors calibrate the count scale in each voltage window.
    const int64_t bare = 5000 +
        (int64_t(w.reference) - w.internal_low) * 9800 / span;
    if (bare < 7000 || bare > 14000)
      return DiodeState::Unknown;
    added[i] = (int64_t(w.diode) - w.reference) * 9800 / span;
  }
  const int64_t curvature = added[0] - added[1];
  // The unused controls measure below GPIO4, with channel-dependent offsets.
  // A near-reference input must be flat; a substantially lower high-window
  // capacitance is also bare. A loaded GPIO4 is rejected above.
  if (added[0] >= -6000 && added[0] <= 1800 &&
      added[1] >= -6000 && added[1] <= 1500 &&
      curvature >= -6000 && (curvature <= 1200 || added[1] <= -1500))
    return DiodeState::Missing;
  if (added[0] >= 4500 && added[0] <= 22000 &&
      added[1] >= 4000 && added[1] <= 18000 &&
      curvature >= 700 && curvature <= 8000)
    return DiodeState::Connected;
  return DiodeState::Unknown;
}

struct DiodeBiasWindow {
  int up = 0;
  int both = 0;
  int down = 0;
};

inline DiodeState diode_bias_state(const DiodeBiasWindow (&windows)[3])
{
  int photo[3];
  int minimum = 10000, maximum = 0;
  bool junction = true;
  for (unsigned i = 0; i < 3; ++i) {
    const auto &w = windows[i];
    photo[i] = w.down;
    if (w.up < minimum) minimum = w.up;
    if (w.up > maximum) maximum = w.up;
    // A forward-biased junction remains near its forward voltage when the
    // second weak pull is added. Photocurrent and resistive loads drop more.
    junction &= w.up >= 400 && w.up <= 1000 && w.down >= -30 &&
                w.down <= 100 && w.up - w.both >= -20 &&
                w.up - w.both <= 35;
  }
  if (diode_voltage_state(photo) == DiodeState::Reversed ||
      (junction && maximum - minimum <= 40))
    return DiodeState::Reversed;
  return DiodeState::Unknown;
}

struct DiodeProbe {
  DiodeState state = DiodeState::Unknown;
  bool receive_safe = false;
};
DiodeState diode_presence_detect(unsigned sensor_pin = 5);
DiodeProbe diode_probe(unsigned sensor_pin = 5);
} // namespace pw_stick
