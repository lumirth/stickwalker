#ifndef PW_STICK_IR_RX_CORE_H
#define PW_STICK_IR_RX_CORE_H

#include <stddef.h>
#include <stdint.h>

namespace pw_stick {

constexpr unsigned kMaxWireBytes = 136;
constexpr unsigned kMaxCaptureBytes = kMaxWireBytes + 2;
constexpr unsigned kMaxBurstGates = 8192;

struct WireBurst {
  uint8_t bytes[kMaxCaptureBytes] = {};
  uint8_t length = 0;
  uint8_t starts = 0;
  uint8_t stops = 0;
  uint16_t weak_pairs = 0;
  uint16_t recovery_tails = 0;
  uint16_t baud_step_10000 = 0;
  int16_t origin_twice = 0;
  uint8_t fit_pass = 0;
  uint32_t fit_visits = 0;
  uint16_t invalid_samples = 0;

  // The decoder checks UART framing. The native protocol checks CONNECT,
  // checksum, command, length and session state.
  bool credible_uart() const {
    return length && stops == length &&
           starts == length;
  }
};

// Decode one bounded optical burst on the foreground task. Each bin is the
// GPIO5 threshold-crossing delay for a 3x-baud gate, 254 for an invalid
// measurement, or 255 for no crossing.
// Static workspace makes calls non-reentrant. The caller supplies the ring,
// burst boundaries and timestamps.
bool decode_wire_burst(const uint8_t *bins, size_t gate_count, WireBurst &out,
                       uint8_t pulse_cutoff = 124);

// Learn only after the caller validates checksum, packet shape and session.
void accept_rx_timing(const WireBurst &burst);
// Clear the timing estimate at session start.
void reset_rx_timing();

}  // namespace pw_stick

#endif
