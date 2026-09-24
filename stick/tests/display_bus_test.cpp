#include "../display_bus.h"

#include <cassert>
#include <cstring>

namespace {

unsigned char pixels[96 * 64];

void command(unsigned char value) {
  IO.PDR1.BIT.B1 = 0;
  StickDisplayWrite(value);
}

void data(unsigned char value) {
  IO.PDR1.BIT.B1 = 1;
  StickDisplayWrite(value);
}

void address(unsigned char column, unsigned char page) {
  command(static_cast<unsigned char>(0x10 | (column >> 4)));
  command(static_cast<unsigned char>(column & 15));
  command(static_cast<unsigned char>(0xb0 | page));
}

void frame() { StickDisplayFrame(pixels, sizeof(pixels)); }

}  // namespace

int main() {
  StickDisplayBusInit();
  assert(SSU.SSSR.BIT.TDRE && SSU.SSSR.BIT.TEND);
  address(5, 0);
  data(0x81);  // High plane: rows 0 and 7.
  data(0x80);  // Low plane: row 7 only.
  frame();
  assert(pixels[5] == 2);
  assert(pixels[7 * 96 + 5] == 3);
  assert(pixels[6 * 96 + 5] == 0);

  // The next pair advances one column, and the second drawing bank stays
  // hidden until the original firmware sends a start-line command.
  data(0xff);
  data(0);
  address(5, 8);
  data(0);
  data(0xff);
  frame();
  assert(pixels[5] == 2);
  assert(pixels[6] == 2);
  command(0x40);
  command(0x40);
  frame();
  assert(pixels[5] == 1);
  assert(pixels[6] == 0);

  // NT7508 RAM spans 128 rows. Start-line wrap exposes the first bank again.
  command(0x40);
  command(127);
  frame();
  assert(pixels[96 + 5] == 2);

  command(0xa9);
  frame();
  for (unsigned char pixel : pixels) assert(pixel == 0);
  command(0xe1);
  frame();
  assert(pixels[96 + 5] == 2);
}
