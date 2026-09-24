#include "../ir_rx_core.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using pw_stick::WireBurst;
using pw_stick::decode_wire_burst;

static void case_for(unsigned length, double phase, uint8_t first = 0x56) {
  uint8_t expected[pw_stick::kMaxWireBytes];
  uint32_t seed = 0x9876abcd;
  for (unsigned i = 0; i < length; ++i) {
    seed = seed * 1664525u + 1013904223u;
    expected[i] = uint8_t(seed >> 24);
  }
  expected[0] = first;  // Raw transport CONNECT is a normal arbitrary byte.
  std::vector<uint8_t> gates(length * 31 + 16, 255);
  for (unsigned byte = 0; byte < length; ++byte) {
    for (unsigned cell = 0; cell < 10; ++cell) {
      const bool pulse = cell == 0 ||
          (cell >= 1 && cell <= 8 &&
           !(expected[byte] & (1u << (cell - 1))));
      if (!pulse) continue;
      const unsigned gate = unsigned(std::lround(
          5 + phase + double(byte * 10 + cell) * 3.004));
      assert(gate < gates.size());
      gates[gate] = 80;
    }
  }
  WireBurst decoded;
  assert(decode_wire_burst(gates.data(), gates.size(), decoded));
  assert(decoded.length == length);
  assert(decoded.starts == length);
  assert(decoded.stops == length);
  assert(decoded.credible_uart());
  for (unsigned i = 0; i < length; ++i) {
    if (decoded.bytes[i] != expected[i])
      std::fprintf(stderr, "length=%u phase=%.1f byte=%u got=%02x want=%02x\n",
                   length, phase, i, decoded.bytes[i], expected[i]);
    assert(decoded.bytes[i] == expected[i]);
  }
}

int main() {
  for (unsigned length : {1u, 2u, 8u, 64u, 136u}) {
    for (double phase : {0.0, 0.2, 0.4, 0.6, 0.8})
      case_for(length, phase);
  }
  // Short control acquisition must not depend on the expected wire value.
  for (unsigned value = 0; value < 256; ++value)
    case_for(1, 0.3, uint8_t(value));
}
