#ifndef PW_STICK_CONTROLS_H
#define PW_STICK_CONTROLS_H

#include <stdint.h>

namespace pw_stick {

constexpr uint8_t kCenter = 0x02;
constexpr uint8_t kLeft = 0x04;
constexpr uint8_t kRight = 0x08;

// Suffixes list the physical Left/Right keys; Center is M+R or M.
enum class Layout : uint8_t { TwoKeyMR, TwoKeyRM, ThreeKeyLR, ThreeKeyRL };
constexpr bool three_key(Layout layout) {
  return layout == Layout::ThreeKeyLR || layout == Layout::ThreeKeyRL;
}
constexpr const char *layout_name(Layout layout) {
  switch (layout) {
    case Layout::TwoKeyMR: return "2-key M/R";
    case Layout::TwoKeyRM: return "2-key R/M";
    case Layout::ThreeKeyLR: return "3-key L/R";
    case Layout::ThreeKeyRL: return "3-key R/L";
  }
  return "2-key M/R";
}

class Controls {
 public:
  static constexpr uint32_t kMaximumPendingAgeMs = 250;
  static constexpr uint8_t kCapacity = 32;
  struct Diagnostic {
    uint8_t gesture, stable, desired, delivered, queued, wait_release;
    uint32_t emitted, consumed, overflow, expired, cancelled;
  };
  void configure(Layout layout);
  void set_chord_window(uint16_t milliseconds);
  void sample(uint32_t now_ms, bool main_pressed, bool side_pressed,
              bool power_pressed, bool captured = false,
              bool power_press_latched = false);
  uint8_t next_scan(uint32_t now_ms);
  bool idle() const;
  uint8_t stable_buttons() const;
  uint8_t pressed_buttons() const;
  bool take_center_press();
  bool take_center_press(uint32_t now_ms);
  bool take_settings_hold();
  void require_release();
  Diagnostic diagnostic() const;

 private:
  enum class Gesture : uint8_t {
    Idle, PendingMain, PendingSide, PendingChord, Main, Side, Chord,
    Suppressed
  };
  struct Debouncer {
    bool stable = false;
    bool candidate = false;
    uint32_t since_ms = 0;
    void update(uint32_t now_ms, bool value);
  };
  void emit(uint8_t value);
  void expire(uint32_t now_ms);
  uint8_t direction(bool main_button) const;

  Layout layout_ = Layout::TwoKeyMR;
  uint16_t chord_window_ms_ = 80;
  Debouncer main_, side_, power_;
  Gesture gesture_ = Gesture::Idle;
  uint32_t first_down_ms_ = 0;
  uint32_t chord_started_ms_ = 0;
  uint8_t chord_first_ = 0;
  uint8_t desired_ = 0;
  uint8_t delivered_ = 0;
  struct Transition {
    uint32_t milliseconds;
    uint8_t value;
  };
  Transition queue_[kCapacity] = {};
  uint8_t head_ = 0;
  uint8_t count_ = 0;
  uint32_t now_ms_ = 0;
  uint8_t pressed_ = 0;
  bool center_notified_ = false;
  bool faulted_ = false;
  bool wait_release_ = true;
  bool settings_hold_tracking_ = false;
  bool settings_hold_requested_ = false;
  uint32_t settings_hold_since_ms_ = 0;
  uint32_t emitted_ = 0, consumed_ = 0, overflow_ = 0;
  uint32_t expired_ = 0, cancelled_ = 0;
};

}  // namespace pw_stick

#endif
