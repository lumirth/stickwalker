#ifndef PW_STICK_IR_RECEIVER_H
#define PW_STICK_IR_RECEIVER_H

#include <stdint.h>

namespace pw_stick {

// Bounds on the CPU-observed crossing delay, in outward-rounded two-cycle
// units: upper in bits 0..6, lower in bits 7..13. Censored observations have
// only a lower bound; invalid observations mark a measurement discontinuity.
using IrObservation = uint16_t;
constexpr unsigned kObservationBoundMask = 0x7f;
constexpr unsigned kObservationLowerShift = 7;
constexpr unsigned kObservationQuantum = 2;
constexpr unsigned kObservationMaxCycles =
    kObservationBoundMask * kObservationQuantum;
constexpr IrObservation kObservationCensored = 0x4000;
constexpr IrObservation kObservationInvalid = 0x8000;

constexpr unsigned IrObservationLower(IrObservation observation) {
  return ((observation >> kObservationLowerShift) & kObservationBoundMask) *
      kObservationQuantum;
}

constexpr unsigned IrObservationUpper(IrObservation observation) {
  return (observation & kObservationBoundMask) * kObservationQuantum;
}

constexpr unsigned kMaxWireBytes = 136;
// A fixed contrast floor, in CPU cycles. Qualification must establish the
// noise and minimum observable pulse margins for this acquisition schedule.
constexpr unsigned kPulseContrastCycles = 6;

struct IrFrame {
  uint8_t bytes[kMaxWireBytes] = {};
  uint8_t length = 0;
  uint32_t end_gate = 0;
  uint16_t end_tick = 0;
};

struct IrReceiverStats {
  uint32_t erasures = 0;
  uint32_t framing_errors = 0;  // Observed pulses wider than two gates.
  uint32_t rejected_origins = 0;
  uint32_t ambiguities = 0;     // Rejected components of overlapping packets.
  uint32_t skipped_origins = 0; // Retired origins, excluding emitted starts.
  uint32_t frames = 0;
  uint32_t overflows = 0;
};

enum IrAdmission : unsigned {
  kIrReject = 0,
  kIrProbe = 1,
  kIrPacket = 2
};

enum IrPollResult : int {
  kIrFailed = -1,
  kIrNeedObservations = 0,
  kIrFrameReady = 1,
  kIrMoreWork = 2
};

// The validator is a pure native-packet check. It must not change session
// state, counters, optical classifications, or UART timing.
using IrValidator = unsigned (*)(const uint8_t *wire, unsigned length);

// Every observed pulse is a possible UART origin. Each origin has one fixed
// byte value and successor, or is invalid. Paths share their common suffix;
// packet admission never changes a pulse classification or a bit period.
class IrReceiver {
 public:
  // reset drops a turn's state but retains lifetime statistics; clear also
  // resets statistics without constructing a large temporary receiver.
  void reset();
  void clear();
  void push(IrObservation observation, uint32_t gate, uint16_t tick);
  IrPollResult poll(IrFrame &completed, IrValidator validator,
                    unsigned work_budget);
  bool active() const { return first_ != produced_ || pulse_length_ || selected_; }
  bool failed() const { return failed_; }
  const IrReceiverStats &stats() const { return stats_; }

 private:
  // A valid 136-byte path contains at most 1224 pulse origins. An unfinished
  // byte adds at most 16; one 1024-gate input batch adds at most 512. The caller
  // must drain kIrMoreWork before supplying another input batch.
  static constexpr unsigned kNodes = 2048;
  static constexpr unsigned kNodeMask = kNodes - 1;
  static constexpr uint8_t kNextMask = 0x1f;
  static constexpr uint8_t kInvalid = 0x40;
  static constexpr uint8_t kReady = 0x80;

  struct ByteNode {
    uint32_t gate = 0;
    uint16_t end_tick = 0;
    uint8_t value = 255;
    uint8_t link = 0;
  };
  static_assert(sizeof(ByteNode) == 8, "bounded UART node storage");

  enum Edge { kPending, kBad, kNext, kEnd };
  ByteNode &node(uint32_t index) { return nodes_[index & kNodeMask]; }
  const ByteNode &node(uint32_t index) const { return nodes_[index & kNodeMask]; }
  void discard_graph();
  void add_pulse(uint32_t gate);
  void finish_nodes(uint32_t center, uint16_t previous_tick);
  Edge edge(uint32_t index) const;
  void retire_origin();
  void select(unsigned strength, uint32_t last);
  bool selected_suffix(uint32_t origin);

  IrObservation observations_[9] = {};
  uint16_t ticks_[9] = {};
  unsigned write_ = 0;
  unsigned history_ = 0;
  uint32_t expected_gate_ = 0;
  bool have_gate_ = false;
  uint32_t center_ = 0;
  uint16_t center_tick_ = 0;
  uint16_t previous_center_tick_ = 0;
  uint32_t pulse_start_ = 0;
  unsigned pulse_length_ = 0;

  ByteNode nodes_[kNodes] = {};
  uint32_t first_ = 0;
  uint32_t produced_ = 0;
  uint32_t unfinished_ = 0;
  bool failed_ = false;

  uint8_t walk_bytes_[kMaxWireBytes] = {};
  uint32_t walk_path_[kMaxWireBytes] = {};
  uint32_t walk_node_ = 0;
  unsigned walk_length_ = 0;
  bool walking_ = false;

  IrFrame chosen_;
  uint32_t chosen_path_[kMaxWireBytes] = {};
  unsigned suffix_cursor_ = 0;
  unsigned selected_ = 0;
  uint32_t cover_end_ = 0;
  bool ambiguous_ = false;
  IrReceiverStats stats_;
};

}  // namespace pw_stick

#endif
