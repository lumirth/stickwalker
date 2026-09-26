#include "../control_settings.h"
#include "../display_palette.h"
#include <algorithm>
#include <cassert>
#include <cstring>

int main() {
  using namespace pw_stick;
  // Migrate every valid original combination without changing its directions.
  for (unsigned old_profile = 0; old_profile < 2; ++old_profile)
    for (unsigned orientation = 0; orientation < 2; ++orientation)
      for (unsigned chord = 0; chord < 3; ++chord) {
        const auto s = migrate_controls(old_profile | orientation << 1 | chord << 2);
        assert(s.orientation == orientation && s.chord_window == chord);
        assert(three_key(s.layout) == bool(old_profile));
        assert(s.layout == (old_profile ? (orientation ? Layout::ThreeKeyLR :
                                                       Layout::ThreeKeyRL)
                                      : (orientation ? Layout::TwoKeyRM :
                                                       Layout::TwoKeyMR)));
      }
  // Every layout can be stored with either rotation and every chord window.
  for (unsigned layout = 0; layout < 4; ++layout)
    for (unsigned orientation = 0; orientation < 2; ++orientation)
      for (unsigned chord = 0; chord < 3; ++chord) {
        const auto s = decode_controls(encode_controls(
            {Layout(layout), uint8_t(orientation), uint8_t(chord)}));
        assert(s.layout == Layout(layout) && s.orientation == orientation &&
               s.chord_window == chord);
      }
  assert(decode_controls(0xff).layout == Layout::TwoKeyMR);
  assert(decode_controls(24).chord_window == 0);
  const char *labels[] = {"2-key M/R", "2-key R/M", "3-key L/R", "3-key R/L"};
  for (unsigned i = 0; i < 4; ++i) assert(!std::strcmp(layout_name(Layout(i)), labels[i]));

  // Four neutral, monotonic colors; exact reverse in light mode. Decode through
  // the RGB565 bit expansion used by the installed graphics library.
  unsigned last = 0;
  for (unsigned shade = 0; shade < 4; ++shade) {
    const uint16_t c = palette_color(true, shade);
    unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    assert(std::max({r, g, b}) - std::min({r, g, b}) <= 1);
    if (shade) assert(r > last);
    last = r;
    assert(c == palette_color(false, 3 - shade));
  }
  assert(palette_color(false, 0) == 0xffff);
  assert(palette_color(false, 3) == 0x0000);
  assert(palette_color(true, 1) == 0x528a);
  assert(palette_color(true, 2) == 0xad75);
}
