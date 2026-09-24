#ifndef PW_STICK_IR_RX_CORE_H
#define PW_STICK_IR_RX_CORE_H

#include <stddef.h>
#include <stdint.h>

namespace pw_stick {

constexpr unsigned kMaxWireBytes = 136;
constexpr unsigned kMaxBurstGates = 8192;

struct WireBurst {
  uint8_t bytes[kMaxWireBytes] = {};
  uint8_t length = 0;
  uint8_t starts = 0;
  uint8_t stops = 0;
  uint16_t weak_pairs = 0;
  uint16_t recovery_tails = 0;

  // Framing alone determines physical acceptability. The native protocol
  // checks CONNECT value, checksum, command, length, and session state.
  bool credible_uart() const {
    return length && stops == length &&
           (starts == length || (length >= 40 && starts + 1 == length));
  }
};

// Decode an already bounded optical burst. Each bin is the measured GPIO5
// threshold-crossing delay for one 3x baud gate, or 255 for no crossing.
// Acquisition and phase fitting consult UART geometry only, never expected
// payload bytes, checksums, or protocol commands. The caller owns the ring,
// optical-gap detection, and timestamps.
bool decode_wire_burst(const uint8_t *bins, size_t gate_count, WireBurst &out);

}  // namespace pw_stick

#endif
