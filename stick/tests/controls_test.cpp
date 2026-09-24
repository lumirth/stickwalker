#include "../controls.h"

#include <cassert>
#include <initializer_list>

using pw_stick::Controls;
using pw_stick::Orientation;
using pw_stick::Profile;

static void step(Controls &controls, uint32_t ms, bool main, bool side,
                 bool power = false) {
  controls.sample(ms, main, side, power);
  controls.sample(ms + 6, main, side, power);
}

static void scans(Controls &controls, std::initializer_list<uint8_t> expected) {
  for (uint8_t value : expected) assert(controls.next_scan() == value);
}

int main() {
  // Either order of the chord must deliver Center without a direction edge.
  for (bool main_first : {false, true}) {
    Controls c;
    c.configure(Profile::Comfort, Orientation::LeftSideDown);
    step(c, 0, false, false);
    step(c, 10, main_first, !main_first);
    step(c, 35, true, true);
    scans(c, {pw_stick::kCenter});
    step(c, 50, false, true);
    step(c, 65, false, false);
    scans(c, {0, 0});
  }

  // A tap between native 62.5 ms scans must survive as press then release.
  {
    Controls c;
    c.configure(Profile::Comfort, Orientation::LeftSideDown);
    step(c, 0, false, false);
    step(c, 10, true, false);
    step(c, 25, false, false);
    scans(c, {pw_stick::kRight, 0});
  }

  // A second button arriving after commitment cannot become Center.
  {
    Controls c;
    c.configure(Profile::Comfort, Orientation::LeftSideDown);
    step(c, 0, false, false);
    step(c, 10, false, true);
    step(c, 100, false, true);
    scans(c, {pw_stick::kLeft});
    step(c, 110, true, true);
    scans(c, {pw_stick::kLeft});
    step(c, 120, true, false);
    scans(c, {0});
    step(c, 130, false, false);
    scans(c, {0});
  }

  // Three-button mode passes simultaneous combinations through unchanged.
  {
    Controls c;
    c.configure(Profile::ThreeButton, Orientation::RightSideDown);
    step(c, 0, false, false);
    step(c, 10, true, true, true);
    scans(c, {pw_stick::kCenter | pw_stick::kLeft | pw_stick::kRight});
  }

  // The comfort directions mirror with the orientation, without changing the
  // M+R center chord.
  {
    Controls c;
    c.configure(Profile::Comfort, Orientation::RightSideDown);
    step(c, 0, false, false);
    step(c, 10, true, false);
    step(c, 100, true, false);
    scans(c, {pw_stick::kLeft});
    step(c, 110, false, false);
    scans(c, {0});
    step(c, 120, true, true);
    scans(c, {pw_stick::kCenter});
  }

  // The left-side-down three-button mapping uses L as native Right.
  {
    Controls c;
    c.configure(Profile::ThreeButton, Orientation::LeftSideDown);
    step(c, 0, false, false);
    step(c, 10, false, true, false);
    scans(c, {pw_stick::kLeft});
    step(c, 20, false, false, true);
    scans(c, {pw_stick::kRight});
  }
}
