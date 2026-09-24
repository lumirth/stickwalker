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
    scans(c, {0});
    step(c, 55, true, true);
    scans(c, {pw_stick::kCenter});
    step(c, 70, false, true);
    step(c, 85, false, false);
    scans(c, {0, 0});
  }

  // Alternating taps may briefly overlap. That overlap must produce the two
  // direction edges in order, never a Center press that enters a menu item.
  {
    Controls c;
    c.configure(Profile::Comfort, Orientation::LeftSideDown);
    step(c, 0, false, false);
    step(c, 10, true, false);
    step(c, 40, true, true);
    step(c, 55, false, true);
    step(c, 90, false, false);
    scans(c, {pw_stick::kLeft, 0, pw_stick::kRight, 0});
  }

  // A tap between native 62.5 ms scans must survive as press then release.
  {
    Controls c;
    c.configure(Profile::Comfort, Orientation::LeftSideDown);
    step(c, 0, false, false);
    step(c, 10, true, false);
    step(c, 25, false, false);
    scans(c, {pw_stick::kLeft, 0});
  }

  // A second button arriving after commitment cannot become Center.
  {
    Controls c;
    c.configure(Profile::Comfort, Orientation::LeftSideDown);
    step(c, 0, false, false);
    step(c, 10, false, true);
    step(c, 100, false, true);
    scans(c, {pw_stick::kRight});
    step(c, 110, true, true);
    scans(c, {pw_stick::kRight});
    step(c, 120, true, false);
    scans(c, {0});
    step(c, 130, false, false);
    scans(c, {0});
  }

  // A wider window changes a near-simultaneous press from one direction into
  // Center, while a genuinely late second press cannot move then confirm.
  {
    Controls c;
    c.configure(Profile::Comfort, Orientation::LeftSideDown);
    c.set_chord_window(120);
    step(c, 0, false, false);
    step(c, 10, false, true);
    step(c, 105, true, true);
    scans(c, {0});
    step(c, 135, true, true);
    scans(c, {pw_stick::kCenter});
    step(c, 160, false, false);
    scans(c, {0});
    step(c, 180, false, true);
    step(c, 310, false, true);
    scans(c, {pw_stick::kRight});
    step(c, 320, true, true);
    scans(c, {pw_stick::kRight});
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
    scans(c, {pw_stick::kRight});
    step(c, 110, false, false);
    scans(c, {0});
    step(c, 120, true, true);
    scans(c, {0});
    step(c, 150, true, true);
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
