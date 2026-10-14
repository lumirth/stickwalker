#include "../diode_feedback.h"
#include "../diode_presence.h"

#include <cassert>
#include <cstdio>
#include <initializer_list>

int main()
{
  using namespace pw_stick;
  // Replays of the live BPV10NF and bare GPIO4 at slope 3, 128 cycles.
  const DiodeCapWindow live[] = {{5792, 3831, 2245, 4536},
                               {20093, 13800, 7985, 17164}};
  assert(diode_cap_state(live) == DiodeState::Connected);
  // The bare input trace replayed at the diode socket is electrically empty.
  const DiodeCapWindow empty[] = {{3831, 3831, 2245, 4536},
                                {13800, 13800, 7985, 17164}};
  assert(diode_cap_state(empty) == DiodeState::Missing);
  const DiodeCapWindow fast[] = {{2548, 1740, 1066, 2067},
                               {8668, 5965, 3502, 7436}};
  assert(diode_cap_state(fast) == DiodeState::Connected);
  // Full-firmware live capture: DAC/channel offsets differ from the isolated
  // probe, but both currents still separate the diode from unloaded GPIO6.
  const DiodeCapWindow native[] = {{1283, 851, 577, 1163},
                                 {4841, 3482, 2028, 4360}};
  const DiodeCapWindow native_fast[] = {{803, 454, 274, 531},
                                      {2389, 1499, 887, 1887}};
  assert(diode_cap_state(native) == DiodeState::Connected);
  assert(diode_cap_state(native_fast) == DiodeState::Connected);
  const DiodeCapWindow bare_control[] = {{811, 830, 576, 1163},
                                       {3255, 3481, 2027, 4361}};
  const DiodeCapWindow bare_control_fast[] = {{395, 453, 274, 530},
                                            {1407, 1498, 887, 1887}};
  assert(diode_cap_state(bare_control) == DiodeState::Missing);
  assert(diode_cap_state(bare_control_fast) == DiodeState::Missing);
  const DiodeCapWindow loaded_reference[] = {{1283, 1400, 577, 1163},
                                           {4841, 4900, 2028, 4360}};
  assert(diode_cap_state(loaded_reference) == DiodeState::Unknown);
  // Clock/gain and reference-capacitor variation must cancel, not change state.
  for (unsigned scale = 50; scale <= 160; ++scale) {
    DiodeCapWindow scaled[2];
    for (unsigned i = 0; i < 2; ++i) {
      scaled[i] = {live[i].diode * scale / 100,
                   live[i].reference * scale / 100,
                   live[i].internal_low * scale / 100,
                   live[i].internal_high * scale / 100};
    }
    assert(diode_cap_state(scaled) == DiodeState::Connected);
  }
  for (unsigned part = 60; part <= 150; ++part) {
    DiodeCapWindow varied[2] = {live[0], live[1]};
    for (unsigned i = 0; i < 2; ++i)
      varied[i].diode = live[i].reference +
          (live[i].diode - live[i].reference) * part / 100;
    assert(diode_cap_state(varied) == DiodeState::Connected);
  }
  // Steady photocurrent slows the correct diode's oscillation. It cannot
  // turn the recorded diode into an empty socket, even when classification
  // becomes inconclusive at extreme illumination.
  for (unsigned light = 100; light <= 300; ++light) {
    DiodeCapWindow lit[2] = {live[0], live[1]};
    for (auto &w : lit)
      w.diode = w.diode * light / 100;
    assert(diode_cap_state(lit) != DiodeState::Missing);
  }
  const DiodeCapWindow broken[] = {{0, 3831, 2245, 2245},
                                 {0, 13800, 7985, 7985}};
  assert(diode_cap_state(broken) == DiodeState::Unknown);
  const DiodeCapWindow flat_capacitor[] = {{5702, 3831, 2245, 4536},
                                        {21293, 13800, 7985, 17164}};
  assert(diode_cap_state(flat_capacitor) == DiodeState::Unknown);
  const DiodeCapWindow gap[] = {{4532, 3831, 2245, 4536},
                              {16610, 13800, 7985, 17164}};
  assert(diode_cap_state(gap) == DiodeState::Unknown);
  // A healthy diode sinks photocurrent with this wiring. With a pull-down,
  // its voltage and an empty socket both remain near zero, including in light.
  // Allow substantial ADC offset/noise without inventing a diagnosis.
  for (int noise = -50; noise <= 50; ++noise) {
    const int healthy[] = {noise, noise + 10, noise - 10};
    assert(diode_voltage_state(healthy) == DiodeState::Unknown);
  }
  const int reversed_light[] = {390, 410, 400};
  assert(diode_voltage_state(reversed_light) == DiodeState::Reversed);
  const int reversed_dark[] = {0, 0, 0};
  assert(diode_voltage_state(reversed_dark) == DiodeState::Unknown);
  const int unstable[] = {250, 410, 400};
  assert(diode_voltage_state(unstable) == DiodeState::Unknown);
  const int shorted[] = {0, 10, 0};
  assert(diode_voltage_state(shorted) == DiodeState::Unknown);
  const int external_voltage[] = {3300, 3300, 3300};
  assert(diode_voltage_state(external_voltage) == DiodeState::Unknown);

  DiodeFeedback feedback;
  assert(feedback.state == DiodeState::Hidden &&
         feedback.expires == UINT64_MAX);
  feedback.show(DiodeState::Connected, 100);
  assert(!feedback.expire(2000099));
  assert(feedback.expire(2000100));
  assert(feedback.state == DiodeState::Hidden &&
         feedback.expires == UINT64_MAX);
  feedback.show(DiodeState::Unknown, 3000000);
  assert(feedback.state == DiodeState::Hidden &&
         feedback.expires == UINT64_MAX);
  assert(!feedback.expire(9000000));
  feedback.show(DiodeState::Missing, 10000000);
  assert(feedback.expire(16000000));

  // Both rotations leave four pixels between the icon and either edge.
  for (unsigned rotation : {0u, 1u}) {
    const unsigned x = diode_icon_x(rotation);
    const unsigned y = diode_icon_y(rotation);
    assert(x >= 4 && x + 16 <= 236);
    assert(x + 16 <= 24 || x >= 216);
    assert(y >= 4 && y + 16 <= 131);
  }
  std::puts("diode feedback: PASS (ambiguous inputs, timeout, both margins)");
}
