#ifndef PW_STICK_CONTROLS_H
#define PW_STICK_CONTROLS_H

#include <stdint.h>

namespace pw_stick {

constexpr uint8_t kCenter = 0x02;
constexpr uint8_t kLeft = 0x04;
constexpr uint8_t kRight = 0x08;

enum class Profile : uint8_t { Comfort, ThreeButton };
enum class Orientation : uint8_t { LeftSideDown, RightSideDown };

// Converts physical M/R/L levels into native Pokewalker button levels. Each
// queued transition is held for one native input scan, including short taps.
class Controls {
 public:
  struct Diagnostic {
    uint8_t gesture, stable, desired, delivered, queued, wait_release;
    uint32_t emitted, consumed, overflow;
  };
  void configure(Profile profile, Orientation orientation);
  void set_chord_window(uint16_t milliseconds);
  void sample(uint32_t now_ms, bool main_pressed, bool side_pressed,
              bool power_pressed);
  uint8_t next_scan();
  bool idle() const;
  bool menu_requested();
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
  uint8_t direction(bool main_button) const;

  Profile profile_ = Profile::Comfort;
  Orientation orientation_ = Orientation::LeftSideDown;
  uint16_t chord_window_ms_ = 80;
  Debouncer main_, side_, power_;
  Gesture gesture_ = Gesture::Idle;
  uint32_t first_down_ms_ = 0;
  uint32_t chord_started_ms_ = 0;
  uint8_t chord_first_ = 0;
  uint8_t desired_ = 0;
  uint8_t delivered_ = 0;
  uint8_t queue_[32] = {};
  uint8_t head_ = 0;
  uint8_t count_ = 0;
  bool menu_ = false;
  bool power_was_down_ = false;
  bool wait_release_ = true;
  uint32_t emitted_ = 0, consumed_ = 0, overflow_ = 0;
};

}  // namespace pw_stick

#endif
