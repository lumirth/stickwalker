#ifndef PW_STICK_IR_RX_CORE_H
#define PW_STICK_IR_RX_CORE_H

#include <stdint.h>

namespace pw_stick {

constexpr unsigned kGateHz = 345600;
constexpr unsigned kMaxWireBytes = 136;
// The native receiver closes a packet after four Timer W ticks without a
// completed UART byte. In particular, the last light pulse is not this clock.
constexpr unsigned kFrameGapGates = 43;
// Fixed local contrast, in gate-readout counts. This requires qualification
// against the supported signal/noise envelope; it is not a per-unit setting.
constexpr uint8_t kMinimumPulseContrast = 6;

struct WireFrame {
  uint8_t bytes[kMaxWireBytes] = {};
  uint8_t length = 0;
  uint8_t minimum_contrast = 255;
  uint32_t first_gate = 0;
  uint32_t last_gate = 0;
};

struct ReceiveEvent {
  bool frame = false;
  bool rejected = false;
  bool gap = false;
};

// Raw acquisition and protocol interpretation are outside this decoder.
// Feed every gate exactly once, including darkness. A nine-gate local upper
// envelope supplies contrast; each UART start establishes a new bit clock.
// Four gates of lookahead require no background/training interval. No global
// workspace, allocation, checksum search, calibration or timing learning.
// A damaged/oversized candidate is discarded through an observed quiet
// delimiter. A packet suffix can never become a new packet after data loss.
//
// A frame contains wire bytes (the caller applies XOR AA). last_gate is the
// final observed stop-completion gate, with one-gate timing quantization.
// 254 is an acquisition erasure. 255 is a censored high/dark reading, clamped
// to the largest ordinary reading, 253; useful contrast must survive it.
class WireReceiver {
 public:
  void reset(uint32_t next_gate = 0);
  void break_capture(uint32_t next_gate) { reset(next_gate); }
  ReceiveEvent feed(uint8_t sample, uint32_t gate, WireFrame &out);

 private:
  void reject(ReceiveEvent &event);
  void start_byte(uint32_t gate, uint8_t contrast);
  void finish_byte(ReceiveEvent &event);
  void pulse(uint32_t gate, uint8_t width, uint8_t contrast,
             ReceiveEvent &event);
  void observation(bool light, uint8_t contrast, uint32_t gate,
                   WireFrame &out, ReceiveEvent &event);

  uint32_t next_gate_ = 0;
  uint8_t window_[9] = {};
  uint8_t window_size_ = 0;
  uint8_t window_next_ = 0;
  bool discarding_ = false;
  uint8_t quiet_gates_ = 0;
  bool in_pulse_ = false;
  uint32_t pulse_gate_ = 0;
  uint8_t pulse_width_ = 0;
  uint8_t pulse_contrast_ = 0;
  bool in_byte_ = false;
  uint32_t byte_gate_ = 0;
  uint16_t occupied_ = 0;
  uint8_t byte_ = 255;
  uint8_t byte_contrast_ = 255;
  WireFrame frame_;
};

}  // namespace pw_stick

#endif
