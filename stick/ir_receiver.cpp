#include "ir_receiver.h"

namespace pw_stick {

void IrReceiver::discard_graph() {
  first_ = produced_ = unfinished_ = 0;
  walking_ = false;
  walk_length_ = 0;
  selected_ = 0;
  suffix_cursor_ = 0;
  ambiguous_ = false;
}

void IrReceiver::reset() {
  history_ = write_ = 0;
  have_gate_ = false;
  pulse_length_ = 0;
  failed_ = false;
  discard_graph();
}

void IrReceiver::clear() {
  reset();
  stats_ = IrReceiverStats{};
}

IrReceiver::Edge IrReceiver::edge(uint32_t index) const {
  const ByteNode &current = node(index);
  if (!(current.link & kReady)) return kPending;
  if (current.link & kInvalid) return kBad;
  const unsigned distance = current.link & kNextMask;
  if (distance) {
    const ByteNode &next = node(index + distance);
    if (next.link & kReady) {
      if (uint16_t(next.end_tick - current.end_tick) > 4) return kEnd;
      return (next.link & kInvalid) ? kBad : kNext;
    }
    // The native poll can close a burst while the next byte is still in
    // flight. At age 29 its completion tick is known but its stop observation
    // still needs one gate: wait, then compare original completion ticks.
    if (center_ - next.gate >= 29) return kPending;
  }
  return uint16_t(center_tick_ - current.end_tick) > 4 ? kEnd : kPending;
}

void IrReceiver::add_pulse(uint32_t gate) {
  if (produced_ - first_ == kNodes) {
    failed_ = true;
    ++stats_.overflows;
    return;
  }
  const uint32_t index = produced_++;
  ByteNode &current = node(index);
  current.gate = gate;
  current.end_tick = 0;
  current.value = 255;
  current.link = 0;

  // Pulse starts are at least two gates apart. Therefore the first successor
  // at delta >=29 is never more than 16 pulse nodes ahead of an origin.
  const unsigned retained = index - first_;
  for (unsigned distance = 1; distance <= 16 && distance <= retained;
       ++distance) {
    ByteNode &previous = node(index - distance);
    if (!(previous.link & kNextMask) && gate - previous.gate >= 29)
      previous.link |= uint8_t(distance);
  }
}

void IrReceiver::finish_nodes(uint32_t center, uint16_t previous_tick) {
  while (unfinished_ != produced_ && center - node(unfinished_).gate >= 30) {
    ByteNode &current = node(unfinished_);
    uint16_t occupied = 1;
    uint8_t value = 255;
    bool valid = true;
    for (uint32_t index = unfinished_ + 1; index != produced_; ++index) {
      const unsigned delta = node(index).gate - current.gate;
      if (delta >= 29) break;
      const unsigned cell = (delta + 1) / 3;
      if (cell == 0 || cell == 9 || (occupied & (1u << cell))) {
        valid = false;
        break;
      }
      occupied |= uint16_t(1u << cell);
      value &= uint8_t(~(1u << (cell - 1)));
    }
    // Finalization is performed on every consecutive center gate. A pulse
    // beginning at 28 and spanning 29 has been delivered before this point.
    current.end_tick = previous_tick;
    current.value = value;
    current.link = uint8_t((current.link & kNextMask) | kReady |
                           (valid ? 0 : kInvalid));
    ++unfinished_;
  }
}

void IrReceiver::retire_origin() {
  ++first_;
  ++stats_.skipped_origins;
  walking_ = false;
  walk_length_ = 0;
}

void IrReceiver::select(unsigned strength, uint32_t last) {
  selected_ = strength;
  ambiguous_ = false;
  chosen_.length = uint8_t(walk_length_);
  for (unsigned i = 0; i < walk_length_; ++i) {
    chosen_.bytes[i] = walk_bytes_[i];
    chosen_path_[i] = walk_path_[i];
  }
  chosen_.end_gate = node(last).gate + 29;
  chosen_.end_tick = node(last).end_tick;
  cover_end_ = chosen_.end_gate;
  suffix_cursor_ = 0;
}

bool IrReceiver::selected_suffix(uint32_t origin) {
  while (suffix_cursor_ < chosen_.length &&
         int32_t(chosen_path_[suffix_cursor_] - origin) < 0)
    ++suffix_cursor_;
  return suffix_cursor_ < chosen_.length &&
      chosen_path_[suffix_cursor_] == origin;
}

IrPollResult IrReceiver::poll(IrFrame &completed, IrValidator validator,
                             unsigned work_budget) {
  if (failed_) return kIrFailed;
  while (work_budget) {
    if (selected_ && (first_ == produced_ ||
                     int32_t(node(first_).gate - cover_end_) > 0)) {
      selected_ = 0;
      if (ambiguous_) {
        ++stats_.ambiguities;
        ambiguous_ = false;
      } else {
        completed = chosen_;
        --stats_.skipped_origins; // Its start was retired while adjudicating.
        ++stats_.frames;
        return kIrFrameReady;
      }
    }
    if (first_ == produced_) return kIrNeedObservations;
    if (!walking_) {
      if (selected_ && selected_suffix(first_)) {
        --work_budget;
        retire_origin();
        continue;
      }
      walking_ = true;
      walk_node_ = first_;
      walk_length_ = 0;
    }

    if (walk_length_ == kMaxWireBytes) {
      // We reached this node only after proving that it is a completed extra
      // byte inside the same native burst. A later root can still be a packet.
      --work_budget;
      ++stats_.rejected_origins;
      retire_origin();
      continue;
    }
    const Edge state = edge(walk_node_);
    if (state == kPending) return kIrNeedObservations;
    --work_budget;
    if (state == kBad) {
      ++stats_.rejected_origins;
      retire_origin();
      continue;
    }
    walk_bytes_[walk_length_] = node(walk_node_).value;
    walk_path_[walk_length_++] = walk_node_;
    if (state == kNext) {
      walk_node_ += node(walk_node_).link & kNextMask;
      continue;
    }

    const unsigned strength = validator(walk_bytes_, walk_length_);
    if (!strength) {
      ++stats_.rejected_origins;
    } else if (!selected_ || strength > selected_) {
      // A checksummed packet dominates an overlapping unprotected FC probe.
      select(strength, walk_node_);
    } else if (strength == selected_) {
      // Exact byte-boundary suffixes were skipped above. Different valid
      // interpretations remain ambiguous even if their values happen to match.
      ambiguous_ = true;
      const uint32_t end = node(walk_node_).gate + 29;
      if (int32_t(end - cover_end_) > 0) cover_end_ = end;
    }
    retire_origin();
  }
  return kIrMoreWork;
}

void IrReceiver::push(IrObservation observation, uint32_t gate, uint16_t tick) {
  if (failed_) return;
  if ((have_gate_ && gate != expected_gate_) ||
      (observation & kObservationInvalid)) {
    ++stats_.erasures;
    // Missing evidence cannot disprove an overlapping interpretation and
    // thereby make a copied candidate uniquely acceptable. Drop the entire
    // uncommitted graph; only frames already returned remain accepted.
    discard_graph();
    history_ = write_ = 0;
    have_gate_ = false;
    pulse_length_ = 0;
    if (observation & kObservationInvalid) return;
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
  if (history_ < 5) return;

  center_ = gate - 4;
  center_tick_ = ticks_[center_slot];
  const IrObservation value = observations_[center_slot];
  unsigned reference_lower = 0;
  for (unsigned i = 0; i < 9; ++i) {
    const unsigned lower_bound = IrObservationLower(observations_[i]);
    if (lower_bound > reference_lower) reference_lower = lower_bound;
  }
  // A reference's lower bound must exceed the current finite upper bound.
  // Overlapping crossing intervals cannot prove a pulse. In particular,
  // polling quantization alone cannot create contrast in a constant signal.
  // A censored current observation cannot prove a pulse at all.
  const bool lit = !(value & kObservationCensored) &&
      reference_lower >= IrObservationUpper(value) + kPulseContrastCycles;
  if (lit) {
    if (!pulse_length_) pulse_start_ = center_;
    if (pulse_length_ < 3) {
      ++pulse_length_;
      if (pulse_length_ == 3) {
        ++stats_.framing_errors;
        // The pulse violates the fixed pulse-width constraint. Drop
        // every uncommitted interpretation in constant time. Keep optical
        // history and the saturated pulse length until this light run ends.
        discard_graph();
      }
    }
  } else if (pulse_length_) {
    if (pulse_length_ <= 2) add_pulse(pulse_start_);
    pulse_length_ = 0;
  }
  if (!failed_) finish_nodes(center_, previous_center_tick_);
  previous_center_tick_ = center_tick_;
}

}  // namespace pw_stick
