#include "controls.h"

namespace pw_stick {

namespace {
// Require sustained overlap so alternating direction taps do not emit Center.
constexpr uint32_t kChordHoldMs = 20;
}

void Controls::Debouncer::update(uint32_t now_ms, bool value) {
  if (value != candidate) {
    candidate = value;
    since_ms = now_ms;
  }
  if (stable != candidate && uint32_t(now_ms - since_ms) >= 5) stable = candidate;
}

void Controls::emit(uint8_t value) {
  if (faulted_ || value == desired_) return;
  if (count_ && queue_[(head_ + count_ - 1) % kCapacity].value == 0 && value) {
    const uint8_t preceding = count_ > 1 ?
        queue_[(head_ + count_ - 2) % kCapacity].value : delivered_;
    if (!(preceding & value)) {
      --count_;
      ++consumed_;
    }
  }
  if (count_ == kCapacity) {
    ++overflow_;
    require_release();
    faulted_ = true;
    return;
  }
  desired_ = value;
  queue_[(head_ + count_) % kCapacity] = {now_ms_, value};
  ++count_;
  ++emitted_;
}

uint8_t Controls::direction(bool main_button) const {
  if (layout_ == Layout::TwoKeyMR)
    return main_button ? kLeft : kRight;
  return main_button ? kRight : kLeft;
}

void Controls::configure(Layout layout) {
  layout_ = layout;
  require_release();
}

void Controls::set_chord_window(uint16_t milliseconds) {
  chord_window_ms_ = milliseconds;
  require_release();
}

void Controls::require_release() {
  gesture_ = Gesture::Suppressed;
  cancelled_ += count_;
  head_ = count_ = 0;
  desired_ = delivered_ = 0;
  center_notified_ = false;
  wait_release_ = true;
}

void Controls::expire(uint32_t now_ms) {
  if (count_ && uint32_t(now_ms - queue_[head_].milliseconds) >
                    kMaximumPendingAgeMs) {
    ++expired_;
    require_release();
    faulted_ = true;
  }
}

void Controls::sample(uint32_t now_ms, bool main_pressed, bool side_pressed,
                      bool power_pressed, bool captured,
                      bool power_press_latched) {
  now_ms_ = now_ms;
  expire(now_ms);
  const uint8_t previous = stable_buttons();
  main_.update(now_ms, main_pressed);
  side_.update(now_ms, side_pressed);
  power_.update(now_ms, power_pressed);
  if (power_press_latched) {
    power_.stable = power_.candidate = true;
    power_.since_ms = now_ms;
  }
  pressed_ = stable_buttons() & ~previous;
  const bool m = main_.stable, r = side_.stable, l = power_.stable;
  if (captured) return;
  if (wait_release_) {
    if (!m && !r && !l && !main_pressed && !side_pressed && !power_pressed) {
      wait_release_ = false;
      faulted_ = false;
      gesture_ = Gesture::Idle;
    }
    return;
  }

  if (three_key(layout_)) {
    uint8_t value = m ? kCenter : 0;
    if (layout_ == Layout::ThreeKeyLR)
      value |= (l ? kLeft : 0) | (r ? kRight : 0);
    else
      value |= (r ? kLeft : 0) | (l ? kRight : 0);
    emit(value);
    return;
  }

  switch (gesture_) {
    case Gesture::Idle:
      if (m && !r) { gesture_ = Gesture::PendingMain; first_down_ms_ = now_ms; }
      else if (r && !m) { gesture_ = Gesture::PendingSide; first_down_ms_ = now_ms; }
      else if (m && r) {
        gesture_ = Gesture::PendingChord;
        chord_first_ = 0;
        chord_started_ms_ = now_ms;
      }
      break;
    case Gesture::PendingMain:
    case Gesture::PendingSide: {
      const bool first_main = gesture_ == Gesture::PendingMain;
      const bool first = first_main ? m : r;
      const bool second = first_main ? r : m;
      if (first && second &&
          uint32_t(now_ms - first_down_ms_) <= chord_window_ms_) {
        gesture_ = Gesture::PendingChord;
        chord_first_ = first_main ? 1 : 2;
        chord_started_ms_ = now_ms;
      } else if (!first) {
        emit(direction(first_main));
        emit(0);
        gesture_ = second ? Gesture::Suppressed : Gesture::Idle;
      } else if (uint32_t(now_ms - first_down_ms_) >= chord_window_ms_) {
        gesture_ = first_main ? Gesture::Main : Gesture::Side;
        emit(direction(first_main));
      }
      break;
    }
    case Gesture::PendingChord:
      if (m && r) {
        if (uint32_t(now_ms - chord_started_ms_) >= kChordHoldMs) {
          gesture_ = Gesture::Chord;
          emit(kCenter);
        }
      } else if (chord_first_ == 1) {
        if (!m) {
          emit(direction(true));
          emit(0);
          gesture_ = r ? Gesture::PendingSide : Gesture::Idle;
          first_down_ms_ = now_ms;
        } else {
          gesture_ = Gesture::Main;
          emit(direction(true));
        }
      } else if (chord_first_ == 2) {
        if (!r) {
          emit(direction(false));
          emit(0);
          gesture_ = m ? Gesture::PendingMain : Gesture::Idle;
          first_down_ms_ = now_ms;
        } else {
          gesture_ = Gesture::Side;
          emit(direction(false));
        }
      } else {
        gesture_ = m ? Gesture::PendingMain :
                   r ? Gesture::PendingSide : Gesture::Idle;
        first_down_ms_ = now_ms;
      }
      break;
    case Gesture::Main:
      if (!m) { emit(0); gesture_ = r ? Gesture::Suppressed : Gesture::Idle; }
      break;
    case Gesture::Side:
      if (!r) { emit(0); gesture_ = m ? Gesture::Suppressed : Gesture::Idle; }
      break;
    case Gesture::Chord:
      if (!m || !r) { emit(0); gesture_ = Gesture::Suppressed; }
      break;
    case Gesture::Suppressed:
      if (!m && !r) gesture_ = Gesture::Idle;
      break;
  }
}

uint8_t Controls::next_scan(uint32_t now_ms) {
  expire(now_ms);
  if (count_) {
    delivered_ = queue_[head_].value;
    head_ = (head_ + 1) % kCapacity;
    --count_;
    ++consumed_;
  } else {
    delivered_ = desired_;
  }
  if (!(delivered_ & kCenter)) center_notified_ = false;
  return delivered_;
}

bool Controls::idle() const {
  return !stable_buttons() && !main_.candidate && !side_.candidate &&
         !power_.candidate && !count_;
}

uint8_t Controls::stable_buttons() const {
  return uint8_t((main_.stable ? 1u : 0u) | (side_.stable ? 2u : 0u) |
                 (power_.stable ? 4u : 0u));
}

uint8_t Controls::pressed_buttons() const { return pressed_; }

Controls::Diagnostic Controls::diagnostic() const {
  return {uint8_t(gesture_),
          stable_buttons(),
          desired_, delivered_, count_, uint8_t(wait_release_),
          emitted_, consumed_, overflow_, expired_, cancelled_};
}

bool Controls::take_center_press() {
  return take_center_press(now_ms_);
}

bool Controls::take_center_press(uint32_t now_ms) {
  expire(now_ms);
  const uint8_t available = count_ ? queue_[head_].value : desired_;
  if (!(available & kCenter) || (delivered_ & kCenter) || center_notified_)
    return false;
  center_notified_ = true;
  return true;
}

}  // namespace pw_stick
