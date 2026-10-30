#ifndef PW_STICK_IR_RECEIVER_H
#define PW_STICK_IR_RECEIVER_H

#include <stdint.h>

namespace pw_stick {

// Finite observations are upper bounds on the threshold-crossing delay.
// Censored observations are lower bounds: the input was still high then.
// Invalid observations indicate a missed deadline or a service discontinuity.
using IrObservation = uint16_t;
constexpr IrObservation kObservationCyclesMask = 0x3fff;
constexpr IrObservation kObservationCensored = 0x4000;
constexpr IrObservation kObservationInvalid = 0x8000;

constexpr unsigned kMaxWireBytes = 136;
// A fixed contrast floor, in CPU cycles. Qualification must establish the
// noise and minimum observable pulse margins for this acquisition schedule.
// This is not an absolute GPIO crossing threshold or a learned sensitivity.
constexpr unsigned kPulseContrastCycles = 6;

struct IrFrame {
  uint8_t bytes[kMaxWireBytes] = {};
  uint8_t length = 0;
  uint32_t end_gate = 0;
  uint16_t end_tick = 0;
  uint32_t discarded_prefix = 0;
};

struct IrReceiverStats {
  uint32_t erasures = 0;
  uint32_t framing_errors = 0;
  uint32_t discarded_bytes = 0;
  uint32_t frames = 0;
};

// Streaming SIR observations -> UART bytes. The caller supplies consecutive
// gate numbers and their original wrapping 32768-Hz Timer-W timestamps,
// independently of when the consumer runs. Each byte has its own start origin.
// No calibration, packet knowledge, checksum search, or learned timing state.
class IrReceiver {
 public:
  // Drop observations at a turn boundary; lifetime statistics are retained.
  void reset();
  bool push(IrObservation observation, uint32_t gate, uint16_t tick,
            IrFrame &completed);
  bool active() const { return byte_active_ || length_ || pulse_length_; }
  const IrReceiverStats &stats() const { return stats_; }

 private:
  void discard_frame();
  void start_byte(uint32_t gate);
  void recover_byte();
  void emit_frame(IrFrame &out);
  void finish_byte(IrFrame &out, bool &emitted);
  void pulse(uint32_t gate, IrFrame &out, bool &emitted);

  IrObservation observations_[9] = {};
  uint16_t ticks_[9] = {};
  unsigned write_ = 0;
  unsigned history_ = 0;
  uint32_t expected_gate_ = 0;
  bool have_gate_ = false;

  uint32_t pulse_start_ = 0;
  unsigned pulse_length_ = 0;
  uint32_t byte_start_ = 0;
  uint16_t byte_end_tick_ = 0;
  uint8_t byte_ = 255;
  uint16_t occupied_ = 0;
  bool byte_active_ = false;
  // At most one pulse can start in each two-gate interval. This bounded
  // history permits a false leading start to be retried after a contradiction.
  uint32_t pulse_gates_[16] = {};
  unsigned pulse_count_ = 0;

  uint8_t bytes_[kMaxWireBytes] = {};
  unsigned first_ = 0;
  unsigned length_ = 0;
  uint32_t end_gate_ = 0;
  uint16_t end_tick_ = 0;
  uint32_t discarded_prefix_ = 0;
  IrReceiverStats stats_;
};

}  // namespace pw_stick

#endif
