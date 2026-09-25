#include "input_bridge.h"

#include "controls.h"
#include "display_bus.h"
#include "foreground_bridge.h"
#ifdef PW_STICK_BENCH_CONTROL
#include "sound_bridge.h"
#endif

#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>

namespace {

pw_stick::Controls controls;
bool power_button_ready = false;
u8 input_profile = 0;
u8 input_orientation = 0;
u8 chord_window_index = 0;
bool menu_active = false;
u8 sampled_raw = 0;
u8 menu_candidate = 0, menu_stable = 0, menu_pending = 0;
uint32_t menu_since_ms = 0, three_menu_since_ms = 0;
bool three_menu_started = false, three_menu_sent = false;
bool three_menu_requested = false;
bool power_event_requested = false;
bool power_action_armed = false;
uint32_t power_released_since_ms = 0;
uint32_t front_wake_since_ms = 0;
bool front_wake_consuming = false;
u8 previous_levels = 0;
#ifdef PW_STICK_BENCH_CONTROL
u8 injected_button = 0;
u8 held_button = 0;
unsigned held_scans = 0;
unsigned main_edges = 0, side_edges = 0, power_edges = 0;
unsigned power_events = 0;
unsigned pmic_errors = 0;
u8 last_raw = 0, last_logical = 0;
unsigned left_edges = 0, right_edges = 0, center_edges = 0;
uint32_t bench_main_until_ms = 0;
bool bench_power_event = false;
bool bench_suppress_until_release = false;
#endif

bool pm1_read(uint8_t reg, uint8_t &value) {
  return m5::In_I2C.readRegister(0x6e, reg, &value, 1, 100000);
}

bool pm1_write(uint8_t reg, uint8_t value) {
  return m5::In_I2C.writeRegister(0x6e, reg, &value, 1, 100000);
}

bool enable_power_button_input() {
  // Keep hardware download-mode recovery available, but move its hold
  // threshold from two to four seconds so the three-button menu shortcut
  // (1.2 seconds) cannot enter it during an ordinary long press.
  uint8_t single, twice, single_after, twice_after;
  if (!pm1_read(0x49, single) || !pm1_read(0x4a, twice)) return false;
  const uint8_t button_config = uint8_t((single & ~0x18u) | 0x18u | 1u);
  if (!pm1_write(0x49, button_config) ||
      !pm1_write(0x4a, uint8_t(twice | 1u)) ||
      !pm1_read(0x49, single_after) || !pm1_read(0x4a, twice_after))
    return false;
  return (single_after & 0x19u) == 0x19u && (twice_after & 1u);
}

}  // namespace

extern "C" void StickInputInit(void) {
  pinMode(11, INPUT_PULLUP);
  pinMode(12, INPUT_PULLUP);
  power_button_ready = enable_power_button_input();
  previous_levels = 0;
  Preferences preferences;
  if (preferences.begin("pw-controls", true)) {
    const u8 stored = preferences.getUChar("layout", 0);
    preferences.end();
    input_profile = stored & 1u;
    input_orientation = (stored >> 1) & 1u;
    chord_window_index = (stored >> 2) & 3u;
    if (chord_window_index > 2) chord_window_index = 0;
  }
  controls.configure(input_profile ? pw_stick::Profile::ThreeButton :
                                     pw_stick::Profile::Comfort,
                     input_orientation ? pw_stick::Orientation::RightSideDown :
                                         pw_stick::Orientation::LeftSideDown);
  controls.set_chord_window(80 + chord_window_index * 40);
}

extern "C" void StickInputPoll(unsigned long milliseconds) {
  uint8_t power_state = 0;
  const bool power_read = power_button_ready && pm1_read(0x48, power_state);
  const bool power_pressed = power_read && (power_state & 1u);
  // BTN_Status bit 7 records a press until the register is read. A short
  // power-key tap can end between polls even when its event was captured.
  const bool power_event = (power_read && (power_state & 0x80u))
#ifdef PW_STICK_BENCH_CONTROL
                           || bench_power_event
#endif
                           ;
#ifdef PW_STICK_BENCH_CONTROL
  bench_power_event = false;
#endif
  // The PMIC may report several state/event changes during one physical tap.
  // Re-arm only after a quiet release, so it cannot open and close the menu
  // on the same gesture.
  if (power_pressed || power_event) {
    power_released_since_ms = 0;
  } else if (!power_released_since_ms) {
    power_released_since_ms = uint32_t(milliseconds);
  } else if (uint32_t(milliseconds - power_released_since_ms) >= 50) {
    power_action_armed = true;
  }
  const bool power_action = power_action_armed &&
                            (power_pressed || power_event);
  if (power_action) power_action_armed = false;
  const bool main_pressed = digitalRead(11) == LOW
#ifdef PW_STICK_BENCH_CONTROL
                            || uint32_t(bench_main_until_ms - milliseconds) <
                                   0x80000000u
#endif
                            ;
  const bool side_pressed = digitalRead(12) == LOW;
  sampled_raw = (main_pressed ? 1u : 0u) | (side_pressed ? 2u : 0u) |
                (power_pressed ? 4u : 0u);
#ifdef PW_STICK_BENCH_CONTROL
  const u8 raw = sampled_raw;
  main_edges += (raw & 1u) && !(last_raw & 1u);
  side_edges += (raw & 2u) && !(last_raw & 2u);
  power_edges += (raw & 4u) && !(last_raw & 4u);
  power_events += power_event;
  pmic_errors += power_button_ready && !power_read;
  last_raw = raw;
  // During the sustained sound test, sample the physical PM1 key but do not
  // dispatch it to the game or to the Stick Settings long-hold shortcut.
  if (StickSoundBenchToneActive()) bench_suppress_until_release = true;
  if (bench_suppress_until_release) {
    if (!sampled_raw) bench_suppress_until_release = false;
    controls.require_release();
    three_menu_started = false;
    three_menu_sent = false;
    three_menu_requested = false;
    power_event_requested = false;
    return;
  }
#endif
  if (menu_active) {
    if (power_action) menu_pending |= 4u;
    if (sampled_raw != menu_candidate) {
      menu_candidate = sampled_raw;
      menu_since_ms = uint32_t(milliseconds);
    }
    if (menu_stable != menu_candidate &&
        uint32_t(milliseconds - menu_since_ms) >= 5) {
      const u8 new_edges = menu_candidate & ~menu_stable;
      menu_stable = menu_candidate;
      menu_pending |= new_edges & 3u;
    }
    return;
  }
  if (front_wake_consuming) {
    if (!main_pressed && !side_pressed && !power_pressed)
      front_wake_consuming = false;
    return;
  }
  if (!StickDisplayIsPowered() && main_pressed && !side_pressed) {
    if (!front_wake_since_ms) {
      front_wake_since_ms = uint32_t(milliseconds);
    } else if (uint32_t(milliseconds - front_wake_since_ms) >= 500) {
      StickForegroundWakeDisplay();
      controls.require_release();
      front_wake_consuming = true;
      front_wake_since_ms = 0;
      return;
    }
  } else {
    front_wake_since_ms = 0;
  }
  if (!input_profile && power_action) power_event_requested = true;
  if (input_profile && power_pressed) {
    if (!three_menu_started) {
      three_menu_started = true;
      three_menu_since_ms = uint32_t(milliseconds);
    } else if (!three_menu_sent &&
               uint32_t(milliseconds - three_menu_since_ms) >= 1200) {
      three_menu_sent = true;
      three_menu_requested = true;
    }
  } else {
    three_menu_started = false;
    three_menu_sent = false;
  }
  controls.sample(uint32_t(milliseconds), main_pressed, side_pressed,
                  input_profile ? power_pressed : false);
}

extern "C" u8 StickInputLevels(void) {
  if (menu_active) {
    previous_levels = 0;
    return 0;
  }
#ifdef PW_STICK_BENCH_CONTROL
  if (held_scans) {
    --held_scans;
    const u8 value = held_button;
    if ((value & pw_stick::kCenter) &&
        !(previous_levels & pw_stick::kCenter))
      StickForegroundCenterWake();
    previous_levels = value;
    return value;
  }
  if (injected_button) {
    const u8 value = injected_button;
    injected_button = 0;
    if ((value & pw_stick::kCenter) &&
        !(previous_levels & pw_stick::kCenter))
      StickForegroundCenterWake();
    previous_levels = value;
    return value;
  }
#endif
  const u8 value = controls.next_scan();
  if ((value & pw_stick::kCenter) &&
      !(previous_levels & pw_stick::kCenter))
    StickForegroundCenterWake();
#ifdef PW_STICK_BENCH_CONTROL
  left_edges += (value & pw_stick::kLeft) &&
                !(previous_levels & pw_stick::kLeft);
  right_edges += (value & pw_stick::kRight) &&
                 !(previous_levels & pw_stick::kRight);
  center_edges += (value & pw_stick::kCenter) &&
                  !(previous_levels & pw_stick::kCenter);
#endif
  previous_levels = value;
#ifdef PW_STICK_BENCH_CONTROL
  last_logical = value;
#endif
  return value;
}

extern "C" int StickInputWakeScanActive(void) {
  if (sampled_raw || !controls.idle() ||
      (power_button_ready && !power_action_armed)) return 1;
#ifdef PW_STICK_BENCH_CONTROL
  if (held_scans || injected_button) return 1;
#endif
  return 0;
}

#ifdef PW_STICK_BENCH_CONTROL
extern "C" void StickInputBenchInject(u8 button) { injected_button = button; }
extern "C" void StickInputBenchMainHold(unsigned milliseconds) {
  bench_main_until_ms = millis() + milliseconds;
}
extern "C" void StickInputBenchPowerEvent(void) { bench_power_event = true; }
extern "C" void StickInputBenchHold(u8 button, unsigned scans) {
  held_button = button;
  held_scans = scans;
}
extern "C" void StickInputDiagnostic(unsigned *main_count,
                                       unsigned *side_count,
                                       unsigned *power_count,
                                       unsigned *power_event_count,
                                       unsigned *pmic_error_count,
                                       unsigned *raw, unsigned *logical,
                                       unsigned *power_ready) {
  *main_count = main_edges;
  *side_count = side_edges;
  *power_count = power_edges;
  *power_event_count = power_events;
  *pmic_error_count = pmic_errors;
  *raw = last_raw;
  *logical = last_logical;
  *power_ready = power_button_ready;
}
extern "C" void StickInputPathDiagnostic(
    unsigned *stable, unsigned *gesture, unsigned *desired,
    unsigned *delivered, unsigned *queued, unsigned *wait_release,
    unsigned *emitted, unsigned *consumed, unsigned *overflow,
    unsigned *left_count, unsigned *right_count, unsigned *center_count) {
  const auto d = controls.diagnostic();
  *stable = d.stable;
  *gesture = d.gesture;
  *desired = d.desired;
  *delivered = d.delivered;
  *queued = d.queued;
  *wait_release = d.wait_release;
  *emitted = d.emitted;
  *consumed = d.consumed;
  *overflow = d.overflow;
  *left_count = left_edges;
  *right_count = right_edges;
  *center_count = center_edges;
}
#endif

extern "C" int StickMenuRequested(void) {
  const bool requested = controls.menu_requested() || three_menu_requested ||
                         power_event_requested;
  three_menu_requested = false;
  power_event_requested = false;
  return requested ? 1 : 0;
}

extern "C" void StickInputMenuMode(int enabled) {
  menu_active = enabled != 0;
  menu_candidate = menu_stable = sampled_raw;
  menu_pending = 0;
  menu_since_ms = millis();
  controls.require_release();
  previous_levels = 0;
}

extern "C" u8 StickInputTakeMenuButtons(void) {
  const u8 value = menu_pending;
  menu_pending = 0;
  return value;
}

extern "C" u8 StickInputProfile(void) { return input_profile; }
extern "C" u8 StickInputOrientation(void) { return input_orientation; }
extern "C" u8 StickInputChordWindowIndex(void) { return chord_window_index; }

extern "C" int StickInputConfigure(u8 profile, u8 orientation,
                                      u8 window_index) {
  if (profile > 1 || orientation > 1 || window_index > 2) return 0;
  Preferences preferences;
  if (!preferences.begin("pw-controls", false)) return 0;
  const size_t written = preferences.putUChar(
      "layout", profile | orientation << 1 | window_index << 2);
  preferences.end();
  if (written != 1) return 0;
  input_profile = profile;
  input_orientation = orientation;
  chord_window_index = window_index;
  controls.configure(profile ? pw_stick::Profile::ThreeButton :
                               pw_stick::Profile::Comfort,
                     orientation ? pw_stick::Orientation::RightSideDown :
                                   pw_stick::Orientation::LeftSideDown);
  controls.set_chord_window(80 + window_index * 40);
  return 1;
}
