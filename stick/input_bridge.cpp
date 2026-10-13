#include "input_bridge.h"

#include "controls.h"
#include "control_settings.h"
#include "display_bus.h"
#include "display_panel.h"
#include "foreground_bridge.h"
#include "sound_bridge.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <esp_timer.h>

namespace {

pw_stick::Controls controls;
bool power_button_ready = false;
pw_stick::ControlSettings settings;
bool menu_active = false;
bool was_m_only_wake = false;
bool discard_power_event = true;
u8 sampled_raw = 0;
u8 menu_pending = 0;
bool menu_wait_release = true;
bool menu_requested = false;
bool power_action_armed = false;
bool power_release_tracking = false;
uint32_t power_released_since_ms = 0;
uint32_t power_retry_ms = 0;
bool wake_center = false;
bool wake_started = false;
bool front_wake_consuming = false;
u8 previous_levels = 0;
uint64_t last_poll_us = 0;
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
  power_retry_ms = millis() + 1000;
  last_poll_us = esp_timer_get_time();
  previous_levels = 0;
  Preferences preferences;
  if (preferences.begin("pw-controls", true)) {
    const u8 stored = preferences.getUChar("layout-v2", 0xff);
    settings = stored == 0xff ?
        pw_stick::migrate_controls(preferences.getUChar("layout", 0)) :
        pw_stick::decode_controls(stored);
    preferences.end();
  }
  controls.configure(settings.layout);
  controls.set_chord_window(80 + settings.chord_window * 40);
}

extern "C" void StickInputPoll(unsigned long milliseconds) {
  const uint32_t now_ms = uint32_t(milliseconds);
  last_poll_us = esp_timer_get_time();
  const bool main_pressed = digitalRead(11) == LOW
#ifdef PW_STICK_BENCH_CONTROL
                            || (bench_main_until_ms &&
                                int32_t(bench_main_until_ms - now_ms) > 0)
#endif
                            ;
  if (StickInputMOnlyWake()) {
    if (!was_m_only_wake) {
      controls.require_release();
      previous_levels = menu_pending = 0;
      menu_requested = power_action_armed = power_release_tracking = false;
      wake_center = wake_started = false;
      front_wake_consuming = false;
      discard_power_event = true;
      was_m_only_wake = true;
    }
    sampled_raw = main_pressed ? 1u : 0u;
    controls.sample(now_ms, main_pressed, false, false, true);
#ifdef PW_STICK_BENCH_CONTROL
    main_edges += main_pressed && !(last_raw & 1u);
    last_raw = sampled_raw;
    bench_power_event = false;
#endif
    if (main_pressed) {
      wake_started = true;
      StickDisplayPrepareWake();
      StickSoundPrepare();
    } else {
      StickDisplayCancelPreparedWake();
    }
    const bool center = (controls.stable_buttons() & 1u) != 0;
    if (center && !wake_center) StickForegroundCenterWake();
    wake_center = center;
    return;
  }
  if (was_m_only_wake) {
    was_m_only_wake = false;
    front_wake_consuming = wake_started;
    wake_center = false;
    controls.require_release();
  }
  if (!power_button_ready && int32_t(now_ms - power_retry_ms) >= 0) {
    power_button_ready = enable_power_button_input();
    power_retry_ms = now_ms + 1000;
    discard_power_event = true;
  }
  uint8_t power_state = 0;
  const bool power_read = power_button_ready && pm1_read(0x48, power_state);
  if (power_button_ready && !power_read) {
#ifdef PW_STICK_BENCH_CONTROL
    ++pmic_errors;
#endif
    power_button_ready = false;
    power_retry_ms = now_ms + 1000;
    power_action_armed = false;
    controls.require_release();
  }
  const bool power_pressed = power_read && (power_state & 1u);
  const bool power_event = !discard_power_event && ((power_read && (power_state & 0x80u))
#ifdef PW_STICK_BENCH_CONTROL
                           || bench_power_event
#endif
                           );
  if (power_read) discard_power_event = false;
#ifdef PW_STICK_BENCH_CONTROL
  bench_power_event = false;
#endif
  if (!power_read || power_pressed || power_event) {
    power_release_tracking = false;
  } else if (!power_release_tracking) {
    power_release_tracking = true;
    power_released_since_ms = now_ms;
  } else if (uint32_t(now_ms - power_released_since_ms) >= 50) {
    power_action_armed = true;
  }
  const bool power_action = power_action_armed &&
                            (power_pressed || power_event);
  if (power_action) power_action_armed = false;
  const bool side_pressed = digitalRead(12) == LOW;
  sampled_raw = (main_pressed ? 1u : 0u) | (side_pressed ? 2u : 0u) |
                (power_pressed ? 4u : 0u);
#ifdef PW_STICK_BENCH_CONTROL
  const u8 raw = sampled_raw;
  main_edges += (raw & 1u) && !(last_raw & 1u);
  side_edges += (raw & 2u) && !(last_raw & 2u);
  power_edges += (raw & 4u) && !(last_raw & 4u);
  power_events += power_event;
  last_raw = raw;
  if (StickSoundBenchToneActive()) bench_suppress_until_release = true;
  if (bench_suppress_until_release) {
    if (!sampled_raw) bench_suppress_until_release = false;
    controls.require_release();
    controls.sample(now_ms, main_pressed, side_pressed, power_pressed, true);
    menu_requested = false;
    return;
  }
#endif
  controls.sample(now_ms, main_pressed, side_pressed, power_pressed,
                  menu_active || front_wake_consuming, power_action);
  if (front_wake_consuming) {
    if (!sampled_raw && !controls.stable_buttons() && power_read) {
      front_wake_consuming = false;
      controls.require_release();
    }
    return;
  }
  if (menu_active) {
    if (menu_wait_release) {
      if (!sampled_raw && !controls.stable_buttons() && power_read)
        menu_wait_release = false;
    } else {
      menu_pending |= controls.pressed_buttons() & 3u;
      if (power_action) menu_pending |= 4u;
    }
    return;
  }
  if (!pw_stick::three_key(settings.layout) && power_action) {
    menu_requested = true;
    controls.require_release();
  }
  if (controls.take_center_press()) StickForegroundCenterWake();
}

extern "C" u8 StickInputLevels(void) {
  if (menu_active || menu_requested || front_wake_consuming ||
      (was_m_only_wake && !StickInputMOnlyWake())) {
    previous_levels = 0;
    return 0;
  }
#ifdef PW_STICK_BENCH_CONTROL
  if (held_scans) {
    --held_scans;
    const u8 value = held_button;
    previous_levels = value;
    return value;
  }
  if (injected_button) {
    const u8 value = injected_button;
    injected_button = 0;
    previous_levels = value;
    return value;
  }
#endif
  u8 value;
  if (StickInputMOnlyWake()) {
    value = wake_center ? pw_stick::kCenter : 0;
  } else {
    const uint32_t now_ms = millis();
    if (controls.take_center_press(now_ms)) StickForegroundCenterWake();
    value = controls.next_scan(now_ms);
  }
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

extern "C" int StickInputMOnlyWake(void) {
  return !StickDisplayIsPowered() && !menu_active;
}

extern "C" int StickInputWakeScanActive(void) {
  if (StickInputMOnlyWake()) return (sampled_raw & 1u) != 0;
  if (sampled_raw || !controls.idle() ||
      (power_button_ready && !power_action_armed)) return 1;
#ifdef PW_STICK_BENCH_CONTROL
  if (held_scans || injected_button) return 1;
#endif
  return 0;
}

extern "C" uint64_t StickInputNextDeadline(void) {
  if (StickInputMOnlyWake() && !sampled_raw &&
      !(controls.stable_buttons() & 1u)) return UINT64_MAX;
  return last_poll_us + 5000;
}

#ifdef PW_STICK_BENCH_CONTROL
extern "C" void StickInputBenchInject(u8 button) {
  injected_button = button;
  if (button & pw_stick::kCenter) StickForegroundCenterWake();
}
extern "C" void StickInputBenchMainHold(unsigned milliseconds) {
  bench_main_until_ms = millis() + milliseconds;
}
extern "C" void StickInputBenchPowerEvent(void) { bench_power_event = true; }
extern "C" void StickInputBenchHold(u8 button, unsigned scans) {
  held_button = button;
  held_scans = scans;
  if (scans && (button & pw_stick::kCenter)) StickForegroundCenterWake();
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
    unsigned *left_count, unsigned *right_count, unsigned *center_count,
    unsigned *expired, unsigned *cancelled) {
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
  *expired = d.expired;
  *cancelled = d.cancelled;
}
#endif

extern "C" int StickMenuRequested(void) {
  const bool requested = menu_requested;
  menu_requested = false;
  return requested ? 1 : 0;
}

extern "C" int StickInputSettingsLauncher(void) {
  if (!pw_stick::three_key(settings.layout) || menu_active ||
      StickInputMOnlyWake()) return 0;
  menu_requested = true;
  controls.require_release();
  return 1;
}

extern "C" void StickInputMenuMode(int enabled) {
  StickForegroundDiscardCenterInput();
  menu_active = enabled != 0;
  menu_pending = 0;
  menu_wait_release = true;
  controls.require_release();
  previous_levels = 0;
}

extern "C" void StickInputRequireRelease(void) {
  controls.require_release();
  StickForegroundDiscardCenterInput();
  previous_levels = 0;
  front_wake_consuming = true;
}

extern "C" u8 StickInputTakeMenuButtons(void) {
  const u8 value = menu_pending;
  menu_pending = 0;
  return value;
}

extern "C" u8 StickInputProfile(void) {
  return pw_stick::three_key(settings.layout);
}
extern "C" u8 StickInputLayout(void) { return u8(settings.layout); }
extern "C" u8 StickInputOrientation(void) { return settings.orientation; }
extern "C" u8 StickInputChordWindowIndex(void) { return settings.chord_window; }

extern "C" int StickInputConfigure(u8 layout, u8 orientation,
                                      u8 window_index) {
  if (layout > 3 || orientation > 1 || window_index > 2) return 0;
  Preferences preferences;
  if (!preferences.begin("pw-controls", false)) return 0;
  const size_t written = preferences.putUChar(
      "layout-v2", pw_stick::encode_controls(
          {pw_stick::Layout(layout), orientation, window_index}));
  preferences.end();
  if (written != 1) return 0;
  settings = {pw_stick::Layout(layout), orientation, window_index};
  controls.configure(settings.layout);
  controls.set_chord_window(80 + settings.chord_window * 40);
  return 1;
}
