#include "display_bus.h"
#include "native_runtime.h"

#include <string.h>

extern "C" {
StickDisplayIo IO = {};
StickDisplaySsu SSU = {};
}

namespace {

constexpr unsigned kColumns = 96;
constexpr unsigned kPages = 16;
uint8_t ram[kPages][kColumns][2];
uint8_t page = 0;
uint8_t column = 0;
uint8_t plane = 0;
uint8_t start_line = 0;
bool start_line_parameter = false;
bool contrast_parameter = false;
bool screen_enabled = true;
uint64_t revision = 0;

void command(uint8_t value) {
  if (start_line_parameter) {
    const uint8_t next = value & 0x7f;
    if (start_line != next) ++revision;
    start_line = next;
    start_line_parameter = false;
    return;
  }
  if (contrast_parameter) {
    contrast_parameter = false;
    return;
  }
  if (value == 0x40) {
    start_line_parameter = true;
  } else if (value == 0x81) {
    contrast_parameter = true;
  } else if (value >= 0xb0 && value <= 0xbf) {
    page = value & 0x0f;
    plane = 0;
  } else if (value >= 0x10 && value <= 0x17) {
    column = uint8_t((column & 0x0f) | ((value & 7u) << 4));
    plane = 0;
  } else if (value <= 0x0f) {
    column = uint8_t((column & 0xf0) | value);
    plane = 0;
  } else if (value == 0xa9) {
    if (screen_enabled) ++revision;
    screen_enabled = false;
  } else if (value == 0xe1 || value == 0xaf) {
    if (!screen_enabled) ++revision;
    screen_enabled = true;
  }
}

void data(uint8_t value) {
  if (page < kPages && column < kColumns) {
    const uint8_t changed = ram[page][column][plane] ^ value;
    ram[page][column][plane] = value;
    if (screen_enabled && changed) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        if ((changed & (1u << bit)) &&
            ((page * 8u + bit - start_line) & 127u) < 64) {
          ++revision;
          break;
        }
      }
    }
  }
  plane ^= 1;
  if (!plane) ++column;
}

}  // namespace

extern "C" void StickDisplayBusInit(void) {
  memset(ram, 0, sizeof(ram));
  IO = StickDisplayIo{};
  SSU = StickDisplaySsu{};
  SSU.SSSR.BIT.TDRE = 1;
  SSU.SSSR.BIT.TEND = 1;
  page = column = plane = start_line = 0;
  start_line_parameter = contrast_parameter = false;
  screen_enabled = true;
  ++revision;
}

extern "C" int StickDisplayIsPowered(void) { return screen_enabled; }
extern "C" uint64_t StickDisplayRevision(void) { return revision; }

extern "C" void StickDisplayCapture(StickDisplaySnapshot *snapshot) {
  if (!snapshot) return;
  snapshot->revision = revision;
  snapshot->powered = screen_enabled;
  StickDisplayFrame(snapshot->pixels, sizeof(snapshot->pixels));
}

extern "C" void StickDisplayWrite(u8 value) {
  StickNativeService();
  if (IO.PDR1.BIT.B1) data(value);
  else command(value);
}

extern "C" void StickDisplayFrame(u8 *pixels, u16 byteCount) {
  if (!pixels || byteCount < kColumns * 64) return;
  for (unsigned y = 0; y < 64; ++y) {
    const unsigned physical_row = (start_line + y) & 127u;
    const unsigned row_page = physical_row >> 3;
    const uint8_t mask = uint8_t(1u << (physical_row & 7u));
    for (unsigned x = 0; x < kColumns; ++x) {
      const uint8_t hi = ram[row_page][x][0] & mask;
      const uint8_t lo = ram[row_page][x][1] & mask;
      pixels[y * kColumns + x] = screen_enabled ?
          uint8_t((hi ? 2u : 0u) | (lo ? 1u : 0u)) : 0;
    }
  }
}
