#include "ir_rx_core.h"

#include "ir_gate.h"

namespace pw_stick {

void WireReceiver::reset(uint32_t next_gate) {
  next_gate_ = next_gate;
  window_size_ = window_next_ = 0;
  in_pulse_ = in_byte_ = discarding_ = false;
  quiet_gates_ = 0;
  frame_ = WireFrame{};
}

void WireReceiver::reject(ReceiveEvent &event) {
  event.rejected = true;
  frame_ = WireFrame{};
  in_byte_ = in_pulse_ = false;
  discarding_ = true;
  quiet_gates_ = 0;
}

void WireReceiver::start_byte(uint32_t gate, uint8_t contrast) {
  in_byte_ = true;
  byte_gate_ = gate;
  byte_ = 255;
  byte_contrast_ = contrast;
  occupied_ = 1;
}

void WireReceiver::finish_byte(ReceiveEvent &event) {
  if (frame_.length == kMaxWireBytes) {
    reject(event);
    return;
  }
  if (!frame_.length) frame_.first_gate = byte_gate_;
  frame_.bytes[frame_.length++] = byte_;
  frame_.last_gate = byte_gate_ + 29;
  if (byte_contrast_ < frame_.minimum_contrast)
    frame_.minimum_contrast = byte_contrast_;
  in_byte_ = false;
}

void WireReceiver::pulse(uint32_t gate, uint8_t width, uint8_t contrast,
                         ReceiveEvent &event) {
  if (width > 2) {
    reject(event);
    return;
  }
  if (!in_byte_) {
    start_byte(gate, contrast);
    return;
  }
  const unsigned cell = (uint32_t(gate - byte_gate_) + 1) / 3;
  if (cell >= 10) {
    finish_byte(event);
    if (!discarding_) start_byte(gate, contrast);
    return;
  }
  if (cell == 9 || (occupied_ & (1u << cell))) {
    reject(event);
    return;
  }
  occupied_ |= uint16_t(1u << cell);
  byte_ &= uint8_t(~(1u << (cell - 1)));
  if (contrast < byte_contrast_) byte_contrast_ = contrast;
}

void WireReceiver::observation(bool light, uint8_t contrast, uint32_t gate,
                               WireFrame &out, ReceiveEvent &event) {
  if (discarding_) {
    // After losing byte phase, even an isolated pulse could be the start of
    // FF. Require a whole possible byte plus the native frame gap in darkness
    // before accepting a new start; never expose an oversized/damaged suffix.
    if (light) quiet_gates_ = 0;
    else if (++quiet_gates_ >= 29 + kFrameGapGates) discarding_ = false;
    return;
  }
  if (light) {
    if (!in_pulse_) {
      in_pulse_ = true;
      pulse_gate_ = gate;
      pulse_width_ = 0;
      pulse_contrast_ = 0;
    }
    if (pulse_width_ < 3 && ++pulse_width_ == 3) reject(event);
    if (contrast > pulse_contrast_) pulse_contrast_ = contrast;
  } else if (in_pulse_) {
    in_pulse_ = false;
    pulse(pulse_gate_, pulse_width_, pulse_contrast_, event);
  }

  // At 3x baud the stop sample is at start+29. A pulse which could still
  // belong to this byte must be resolved before accepting its framing.
  // A start at +29 belongs to the next byte, even at the fast clock corner.
  if (in_byte_ && uint32_t(gate - byte_gate_) >= 29 &&
      (!in_pulse_ || uint32_t(pulse_gate_ - byte_gate_) >= 29))
    finish_byte(event);

  // A new byte may already be underway when an inter-packet gap expires.
  // Keep that byte; only transfer the preceding completed packet to out.
  if (frame_.length && uint32_t(gate - frame_.last_gate) >= kFrameGapGates) {
    out = frame_;
    frame_ = WireFrame{};
    event.frame = true;
  }
}

ReceiveEvent WireReceiver::feed(uint8_t sample, uint32_t gate, WireFrame &out) {
  ReceiveEvent event;
  if (gate != next_gate_) {
    reset(gate);
    discarding_ = true;
    event.gap = true;
  }
  next_gate_ = gate + 1;
  if (sample == kInvalidGate) {
    reset(next_gate_);
    discarding_ = true;
    event.gap = true;
    return event;
  }
  if (sample == 255) sample = 253;
  if (!window_size_) {
    // Extend the first observed sample to the left. This permits a start
    // pulse in the very first gate without inventing a dark preamble.
    for (unsigned i = 0; i < 4; ++i) window_[i] = sample;
    window_size_ = window_next_ = 4;
  }
  window_[window_next_] = sample;
  if (++window_next_ == 9) window_next_ = 0;
  if (window_size_ < 9 && ++window_size_ < 9) return event;
  uint8_t upper = 0;
  for (uint8_t value : window_)
    if (value > upper) upper = value;
  unsigned center = window_next_ + 4;
  if (center >= 9) center -= 9;
  const uint8_t contrast = upper - window_[center];
  observation(contrast >= kMinimumPulseContrast, contrast, gate - 4, out,
              event);
  return event;
}

}  // namespace pw_stick
