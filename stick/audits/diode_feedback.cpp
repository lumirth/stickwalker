#include "../diode_feedback.h"

#include <cassert>
#include <cstdio>
#include <initializer_list>

int main()
{
  using namespace pw_stick;
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
  assert(feedback.expire(9000000));

  // Both rotations leave four pixels between the icon and either edge.
  for (unsigned rotation : {0u, 1u}) {
    const unsigned x = diode_icon_x(rotation);
    assert(x >= 4 && x + 16 <= 236);
    assert(x + 16 <= 24 || x >= 216);
    assert(kDiodeIconY >= 4 && kDiodeIconY + 16 <= 131);
  }
  std::puts("diode feedback: PASS (ambiguous inputs, timeout, both margins)");
}
