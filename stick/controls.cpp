#include "controls.h"

namespace pw_stick {

void Controls::Debouncer::update(uint32_t now_ms, bool value) {
  if (value != candidate) {
    candidate = value;
    since_ms = now_ms;
  }
  if (stable != candidate && uint32_t(now_ms - since_ms) >= 5) stable = candidate;
}

void Controls::emit(uint8_t value) {
  if (value == desired_) return;
  desired_ = value;
  if (count_ < sizeof(queue_)) {
    queue_[(head_ + count_) % sizeof(queue_)] = value;
    ++count_;
  }
}

uint8_t Controls::direction(bool main_button) const {
  if (orientation_ == Orientation::LeftSideDown)
    return main_button ? kRight : kLeft;
  return main_button ? kLeft : kRight;
}

void Controls::configure(Profile profile, Orientation orientation) {
  profile_ = profile;
  orientation_ = orientation;
  require_release();
}

void Controls::require_release() {
  gesture_ = Gesture::Suppressed;
  head_ = count_ = 0;
  desired_ = delivered_ = 0;
  menu_ = false;
  power_was_down_ = power_.stable;
  wait_release_ = true;
}

void Controls::sample(uint32_t now_ms, bool main_pressed, bool side_pressed,
                      bool power_pressed) {
  main_.update(now_ms, main_pressed);
  side_.update(now_ms, side_pressed);
  power_.update(now_ms, power_pressed);
  const bool m = main_.stable, r = side_.stable, l = power_.stable;
  if (wait_release_) {
    if (!m && !r && !l) {
      wait_release_ = false;
      gesture_ = Gesture::Idle;
    }
    return;
  }

  if (profile_ == Profile::ThreeButton) {
    uint8_t value = m ? kCenter : 0;
    if (orientation_ == Orientation::LeftSideDown)
      value |= (r ? kLeft : 0) | (l ? kRight : 0);
    else
      value |= (l ? kLeft : 0) | (r ? kRight : 0);
    emit(value);
    return;
  }

  if (l && !power_was_down_) menu_ = true;
  power_was_down_ = l;
  switch (gesture_) {
    case Gesture::Idle:
      if (m && !r) { gesture_ = Gesture::PendingMain; first_down_ms_ = now_ms; }
      else if (r && !m) { gesture_ = Gesture::PendingSide; first_down_ms_ = now_ms; }
      else if (m && r) { gesture_ = Gesture::Chord; emit(kCenter); }
      break;
    case Gesture::PendingMain:
    case Gesture::PendingSide: {
      const bool first_main = gesture_ == Gesture::PendingMain;
      const bool first = first_main ? m : r;
      const bool second = first_main ? r : m;
      if (first && second && uint32_t(now_ms - first_down_ms_) <= 80) {
        gesture_ = Gesture::Chord;
        emit(kCenter);
      } else if (!first) {
        emit(direction(first_main));
        emit(0);
        gesture_ = second ? Gesture::Suppressed : Gesture::Idle;
      } else if (uint32_t(now_ms - first_down_ms_) >= 80) {
        gesture_ = first_main ? Gesture::Main : Gesture::Side;
        emit(direction(first_main));
      }
      break;
    }
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

uint8_t Controls::next_scan() {
  if (count_) {
    delivered_ = queue_[head_];
    head_ = (head_ + 1) % sizeof(queue_);
    --count_;
  } else {
    delivered_ = desired_;
  }
  return delivered_;
}

bool Controls::idle() const { return !main_.stable && !side_.stable && !power_.stable && !count_; }

bool Controls::menu_requested() {
  bool requested = menu_;
  menu_ = false;
  return requested;
}

}  // namespace pw_stick
