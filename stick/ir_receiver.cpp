#include "ir_receiver.h"

namespace pw_stick {

void IrReceiver::discard_frame() {
  byte_active_ = false;
  pulse_count_ = 0;
  first_ = 0;
  length_ = 0;
  discarded_prefix_ = 0;
}

void IrReceiver::reset() {
  history_ = 0;
  write_ = 0;
  have_gate_ = false;
  pulse_length_ = 0;
  discard_frame();
}

void IrReceiver::start_byte(uint32_t gate) {
  byte_active_ = true;
  byte_start_ = gate;
  byte_ = 255;
  occupied_ = 1;
  pulse_count_ = 1;
  pulse_gates_[0] = gate;
}

void IrReceiver::emit_frame(IrFrame &out) {
  out.length = uint8_t(length_);
  for (unsigned i = 0; i < length_; ++i)
    out.bytes[i] = bytes_[(first_ + i) % kMaxWireBytes];
  out.end_gate = end_gate_;
  out.end_tick = end_tick_;
  out.discarded_prefix = discarded_prefix_;
  first_ = 0;
  length_ = 0;
  discarded_prefix_ = 0;
  ++stats_.frames;
}

void IrReceiver::finish_byte(IrFrame &out, bool &emitted) {
  // UART completion takes priority over the native idle check when both are
  // observed together. Compare original timestamps, not consumer latency.
  if (length_ && uint16_t(byte_end_tick_ - end_tick_) > 4) {
    emit_frame(out);
    emitted = true;
  }
  if (length_ == kMaxWireBytes) {
    first_ = (first_ + 1) % kMaxWireBytes;
    --length_;
    ++discarded_prefix_;
    ++stats_.discarded_bytes;
  }
  bytes_[(first_ + length_) % kMaxWireBytes] = byte_;
  ++length_;
  end_gate_ = byte_start_ + 29;
  end_tick_ = byte_end_tick_;
  byte_active_ = false;
  pulse_count_ = 0;
}

void IrReceiver::recover_byte() {
  ++stats_.framing_errors;
  first_ = 0;
  length_ = 0;
  discarded_prefix_ = 0;

  // Retry only when UART framing contradicts the current origin. At most 16
  // starts and 16 observed pulses are considered; ordinary bytes never search.
  // Noise that itself forms valid UART remains intrinsically ambiguous and
  // must be rejected by the independent native packet/state checks.
  for (unsigned first = 1; first < pulse_count_; ++first) {
    uint8_t value = 255;
    uint16_t occupied = 1;
    bool valid = true;
    for (unsigned i = first + 1; i < pulse_count_; ++i) {
      const unsigned cell = (pulse_gates_[i] - pulse_gates_[first] + 1) / 3;
      if (cell == 0 || cell >= 9 || (occupied & (1u << cell))) {
        valid = false;
        break;
      }
      occupied |= uint16_t(1u << cell);
      value &= uint8_t(~(1u << (cell - 1)));
    }
    if (!valid) continue;
    byte_start_ = pulse_gates_[first];
    byte_ = value;
    occupied_ = occupied;
    pulse_count_ -= first;
    for (unsigned i = 0; i < pulse_count_; ++i)
      pulse_gates_[i] = pulse_gates_[first + i];
    return;
  }
  byte_active_ = false;
  pulse_count_ = 0;
}

void IrReceiver::pulse(uint32_t gate, IrFrame &out, bool &emitted) {
  if (!byte_active_) {
    start_byte(gate);
    return;
  }
  const unsigned cell = (gate - byte_start_ + 1) / 3;
  if (cell >= 10) {
    finish_byte(out, emitted);
    start_byte(gate);
    return;
  }
  if (pulse_count_ == 16) {
    ++stats_.framing_errors;
    discard_frame();
    start_byte(gate);
    return;
  }
  pulse_gates_[pulse_count_++] = gate;
  if (cell == 0 || cell == 9 || (occupied_ & (1u << cell))) {
    recover_byte();
    return;
  }
  occupied_ |= uint16_t(1u << cell);
  byte_ &= uint8_t(~(1u << (cell - 1)));
}

bool IrReceiver::push(IrObservation observation, uint32_t gate, uint16_t tick,
                      IrFrame &completed) {
  if ((have_gate_ && gate != expected_gate_) ||
      (observation & kObservationInvalid)) {
    ++stats_.erasures;
    reset();
    if (observation & kObservationInvalid) return false;
  }
  have_gate_ = true;
  expected_gate_ = gate + 1;
  if (!history_) {
    // Edge extension needs no earlier dark history or training interval.
    for (unsigned i = 0; i < 9; ++i) {
      observations_[i] = observation;
      ticks_[i] = tick;
    }
  }
  observations_[write_] = observation;
  ticks_[write_] = tick;
  if (history_ < 9) ++history_;
  const unsigned center_slot = write_ >= 4 ? write_ - 4 : write_ + 5;
  write_ = write_ == 8 ? 0 : write_ + 1;
  if (history_ < 5) return false;

  const uint32_t center = gate - 4;
  const uint16_t center_tick = ticks_[center_slot];
  const IrObservation value = observations_[center_slot];
  unsigned upper = 0;
  for (unsigned i = 0; i < 9; ++i) {
    const unsigned lower_bound = observations_[i] & kObservationCyclesMask;
    if (lower_bound > upper) upper = lower_bound;
  }
  // A censored observation supplies only a lower bound on the dark envelope.
  // It cannot itself prove a light pulse. In particular, no-crossing is never
  // converted to a fabricated numerical crossing delay such as 255 cycles.
  const bool lit = !(value & kObservationCensored) &&
      upper >= unsigned(value & kObservationCyclesMask) + kPulseContrastCycles;
  bool emitted = false;

  // Native UART polling can close an old frame while another byte is still
  // arriving. One extra gate is allowed to validate a pending stop observation;
  // finish_byte then compares the byte's original completion timestamp.
  if (length_ && uint16_t(center_tick - end_tick_) > 4 &&
      (!byte_active_ || center - byte_start_ < 29)) {
    emit_frame(completed);
    emitted = true;
  }

  if (lit) {
    if (!pulse_length_) pulse_start_ = center;
    if (pulse_length_ < 3) ++pulse_length_;
    if (pulse_length_ == 3) {
      if (byte_active_ || length_) ++stats_.framing_errors;
      discard_frame();
    }
  } else if (pulse_length_) {
    if (pulse_length_ <= 2) pulse(pulse_start_, completed, emitted);
    pulse_length_ = 0;
  }

  if (byte_active_) {
    const uint32_t age = center - byte_start_;
    if (age == 29) byte_end_tick_ = center_tick;
    // A late stop-cell pulse may occupy gates 28 and 29. Wait for gate 30
    // before committing the byte, so that pulse cannot escape framing checks.
    if (age >= 30) finish_byte(completed, emitted);
  }
  return emitted;
}

}  // namespace pw_stick
