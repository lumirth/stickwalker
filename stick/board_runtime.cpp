#include "display_panel.h"
#include "display_bus.h"
#include "board_hal.h"
#include "battery_bridge.h"
#include "eeprom_backend.h"
#include "foreground_bridge.h"
#include "input_bridge.h"
#include "ir_transport.h"
#include "sound_bridge.h"
#include "power_sleep.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <esp_timer.h>

extern "C" void StickPortBoot(void);
#ifdef PW_STICK_BENCH_CONTROL
extern "C" void StickPortTrace(const char *message) {
  if (StickIrBenchTraceEnabled()) Serial.println(message);
}
#endif

namespace {

bool ready = false;
bool device_menu_open = false;
u8 device_menu_row = 0;
bool previous_ir = false;
unsigned last_present_frame = ~0u;
int last_present_power = -1;
#ifdef PW_STICK_BENCH_CONTROL
bool passive_capture = false;
uint64_t passive_deadline_us = 0;
bool receive_trial = false;
uint64_t receive_deadline_us = 0;
uint64_t timing_started_us = 0;
uint64_t main_ticks = 0, beep_ticks = 0, input_polls = 0;
uint64_t main_us = 0, beep_us = 0, input_us = 0, present_us = 0;
uint64_t sound_us = 0, sound_max_us = 0, loop_gap_max_us = 0;
uint64_t last_loop_entry_us = 0;
#endif
uint64_t next_sample_us = 0;
uint64_t next_quarter_us = 0;
uint64_t next_second_us = 0;
uint64_t next_input_us = 0;
uint64_t next_beep_us = 0;
uint64_t next_vbus_check_us = 0;
uint64_t last_battery_draw_us = 0;
bool usb_present = true;
#ifdef PW_STICK_BENCH_CONTROL
uint64_t usb_sleep_trial_end_us = 0;
#endif

uint64_t sample_period_us() {
  // The H8 disables regular 16 Hz sampling after its motion timeout and
  // collects one activity sample on each RTC second instead.
  return StickForegroundIsInactive() && !StickInputWakeScanActive() ?
      1000000 : 62500;
}

void fatal(const char *reason) {
  Serial.printf("PW_STICK_FATAL %s\n", reason);
  if (auto *screen = StickBoardScreen()) {
    screen->fillScreen(TFT_BLACK);
    screen->setTextColor(TFT_WHITE, TFT_BLACK);
    screen->setCursor(8, 12);
    screen->print(reason);
  }
}

int battery_percent_estimate(u16 millivolts) {
  if (!millivolts) return -1;
  // M5Unified uses this voltage-only scale for M5PM1. It is an estimate,
  // particularly while charging or under a changing load.
  if (millivolts <= 3300) return 0;
  if (millivolts >= 4100) return 100;
  return (millivolts - 3300) * 100 / 800;
}

void draw_battery_readout() {
  auto *screen = StickBoardScreen();
  if (!screen) return;
  const u16 millivolts = StickBatteryMillivolts();
  const int percent = battery_percent_estimate(millivolts);
  const bool on_usb = StickBoardPower().getVBUSVoltage() >= 4000;
  screen->fillRect(8, 27, 224, 12, TFT_BLACK);
  screen->setTextColor(TFT_WHITE, TFT_BLACK);
  screen->setTextSize(1);
  screen->setCursor(10, 28);
  if (percent < 0) {
    screen->print("Battery unavailable");
  } else {
    screen->printf("Battery %u.%02u V  ~%d%%  %s", unsigned(millivolts / 1000),
                   unsigned((millivolts % 1000) / 10), percent,
                   on_usb ? "USB" : "");
  }
  last_battery_draw_us = uint64_t(esp_timer_get_time());
}

void draw_device_menu() {
  auto *screen = StickBoardScreen();
  if (!screen) return;
  StickDisplayPanelSetBacklight(1);
  screen->fillScreen(TFT_BLACK);
  screen->setTextColor(TFT_WHITE, TFT_BLACK);
  screen->setTextSize(2);
  screen->setCursor(8, 8);
  screen->print("STICK SETTINGS");
  draw_battery_readout();
  screen->setTextSize(1);
  screen->setCursor(10, 42);
  screen->printf("%c Input: %s", device_menu_row == 0 ? '>' : ' ',
                 StickInputProfile() ? "Three buttons" : "Comfort M+R");
  screen->setCursor(10, 59);
  screen->printf("%c Rotation: %s", device_menu_row == 1 ? '>' : ' ',
                 StickInputOrientation() ? "Right side down" : "Left side down");
  screen->setCursor(10, 76);
  screen->printf("%c Chord window: %u ms", device_menu_row == 2 ? '>' : ' ',
                 80 + StickInputChordWindowIndex() * 40);
  screen->setCursor(10, 93);
  screen->printf("%c Test speaker", device_menu_row == 3 ? '>' : ' ');
  screen->drawFastHLine(8, 107, 224, TFT_WHITE);
  screen->setCursor(10, 115);
  screen->print("M next  R change  L close");
}

void present_game_if_changed(bool force = false) {
  if (device_menu_open) return;
  const unsigned frame = StickForegroundUiFrame();
  const int power = StickDisplayIsPowered();
  if (!force && last_present_frame == frame && last_present_power == power)
    return;
  StickDisplayPresent();
  last_present_frame = frame;
  last_present_power = power;
}

}  // namespace

extern "C" void StickPortSetup(void) {
  Serial.begin(115200);
  setCpuFrequencyMhz(240);
  if (!StickBoardBegin()) { fatal("Board init failed"); return; }
  if (!StickDisplayPanelInit()) { fatal("Display buffer failed"); return; }
  if (!StickEepromMount()) { fatal("EEPROM mount failed"); return; }
  StickEepromDefer(1);
  StickPortBoot();
  StickEepromDefer(0);
  StickDisplayPanelSetOrientation(StickInputOrientation());
  present_game_if_changed(true);
  // The external 5 V rail feeds the board IR hardware, not the bare GPIO5
  // photodiode. It is restored by StickIrConfigure when a session starts.
  StickBoardPower().setExtOutput(false);
  StickSleepBegin();
  const uint64_t now = uint64_t(esp_timer_get_time());
  usb_present = StickBoardPower().getVBUSVoltage() >= 4000;
  next_vbus_check_us = now + 1000000;
  next_sample_us = now + sample_period_us();
  next_quarter_us = now + 250000;
  next_second_us = now + 1000000;
  next_input_us = now + 5000;
  next_beep_us = now + 8000;
#ifdef PW_STICK_BENCH_CONTROL
  timing_started_us = now;
#endif
  ready = true;
#ifdef PW_STICK_BENCH_CONTROL
  Serial.println("PW_STICK_READY");
#endif
}

extern "C" void StickPortLoop(void) {
  if (!ready) { delay(50); return; }
  const uint64_t now = uint64_t(esp_timer_get_time());
#ifdef PW_STICK_BENCH_CONTROL
  if (last_loop_entry_us && now - last_loop_entry_us > loop_gap_max_us)
    loop_gap_max_us = now - last_loop_entry_us;
  last_loop_entry_us = now;
  if (passive_capture) {
    if (now >= passive_deadline_us) {
      StickIrStop();
      StickIrPassiveDiagnostic(0);
      passive_capture = false;
      Serial.println("PW_STICK_PASSIVE_DONE");
      StickIrBenchTrace(0);
    } else {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    }
    return;
  }
  if (receive_trial) {
    u8 wire[136], length;
    u16 tick;
    while (StickIrTakeBurst(wire, sizeof(wire), &length, &tick)) {
      Serial.printf("PW_STICK_TRIAL_FRAME length=%u tick=%u wire=", length,
                    tick);
      for (unsigned i = 0; i < length; ++i) Serial.printf("%02x", wire[i]);
      Serial.println();
    }
    if (now >= receive_deadline_us || StickIrFailed()) {
      StickIrStop();
      receive_trial = false;
      Serial.println("PW_STICK_TRIAL_DONE");
      StickIrBenchTrace(0);
    } else {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
    }
    return;
  }
  while (Serial.available()) {
    const int command = Serial.read();
    if (command == 'c' && !StickForegroundIsIr()) {
      StickIrBenchTrace(1);
      StickForegroundRequestIr();
      Serial.println("PW_STICK_BENCH_CONNECT");
      if (!StickForegroundIsIr()) StickIrBenchTrace(0);
    }
    if (command == 'p' && !StickForegroundIsIr()) {
      StickIrBenchTrace(1);
      StickIrConfigure();
      StickIrPassiveDiagnostic(1);
      StickIrStart();
      passive_capture = !StickIrFailed();
      passive_deadline_us = uint64_t(esp_timer_get_time()) + 200000;
      Serial.println(passive_capture ? "PW_STICK_PASSIVE_START" :
                     "PW_STICK_PASSIVE_ERROR");
      if (!passive_capture) {
        StickIrPassiveDiagnostic(0);
        StickIrBenchTrace(0);
      }
    }
    if (command == 'r' && !StickForegroundIsIr()) {
      StickIrBenchTrace(1);
      StickIrConfigure();
      StickIrPassiveDiagnostic(0);
      StickIrStart();
      receive_trial = !StickIrFailed();
      receive_deadline_us = uint64_t(esp_timer_get_time()) + 2000000;
      Serial.println(receive_trial ? "PW_STICK_TRIAL_START" :
                     "PW_STICK_TRIAL_ERROR");
      if (!receive_trial) StickIrBenchTrace(0);
    }
    if (command == 'e' && !StickForegroundIsIr()) {
      const unsigned char *snapshot = StickEepromSnapshot();
      if (!snapshot) {
        Serial.println("PW_STICK_EEPROM_UNAVAILABLE");
      } else {
        static constexpr char hex[] = "0123456789abcdef";
        char row[513];
        Serial.println("PW_STICK_EEPROM_BEGIN bytes=65536");
        for (unsigned offset = 0; offset < 65536; offset += 256) {
          for (unsigned i = 0; i < 256; ++i) {
            const unsigned char value = snapshot[offset + i];
            row[2 * i] = hex[value >> 4];
            row[2 * i + 1] = hex[value & 15];
          }
          row[512] = 0;
          Serial.printf("PW_STICK_EEPROM_DATA offset=%u %s\n", offset, row);
        }
        Serial.println("PW_STICK_EEPROM_END");
      }
    }
    if (command == 'f' && !StickForegroundIsIr()) {
      static uint8_t pixels[96 * 64];
      static constexpr char hex[] = "0123456789abcdef";
      char row[97];
      StickDisplayFrame(pixels, sizeof(pixels));
      Serial.println("PW_STICK_FRAME_BEGIN width=96 height=64");
      for (unsigned y = 0; y < 64; ++y) {
        for (unsigned x = 0; x < 96; x += 2)
          row[x / 2] = hex[(pixels[y * 96 + x] << 2) |
                             pixels[y * 96 + x + 1]];
        row[48] = 0;
        Serial.printf("PW_STICK_FRAME_ROW y=%u %s\n", y, row);
      }
      Serial.println("PW_STICK_FRAME_END");
    }
    if (command == 'i' && !StickForegroundIsIr()) {
      uint8_t pmic = 0;
      uint8_t reset_cfg = 0, off_cfg = 0;
      const bool pmic_ok = m5::In_I2C.readRegister(0x6e, 0x48, &pmic, 1,
                                                  100000);
      const bool config_ok = m5::In_I2C.readRegister(0x6e, 0x49,
                                                    &reset_cfg, 1, 100000) &&
                             m5::In_I2C.readRegister(0x6e, 0x4a,
                                                    &off_cfg, 1, 100000);
      unsigned m_edges, r_edges, l_edges, l_events, pmic_errors, raw, logical, power_ready;
      StickInputDiagnostic(&m_edges, &r_edges, &l_edges, &l_events, &pmic_errors,
                           &raw, &logical, &power_ready);
      Serial.printf("PW_STICK_INPUT_RAW main=%u side=%u power=%u pmic_ok=%u pmic=%02x config_ok=%u reset_cfg=%02x off_cfg=%02x\n",
                    unsigned(digitalRead(11) == LOW),
                    unsigned(digitalRead(12) == LOW),
                    unsigned(pmic_ok && (pmic & 1)), unsigned(pmic_ok), pmic,
                    unsigned(config_ok), reset_cfg, off_cfg);
      Serial.printf("PW_STICK_INPUT_EVENTS main=%u side=%u power=%u power_events=%u pmic_errors=%u raw=%u logical=%u power_ready=%u\n",
                    m_edges, r_edges, l_edges, l_events, pmic_errors, raw, logical,
                    power_ready);
      unsigned stable, gesture, desired, delivered, queued, wait_release;
      unsigned emitted, consumed, overflow, left, right, center;
      StickInputPathDiagnostic(&stable, &gesture, &desired, &delivered,
                               &queued, &wait_release, &emitted, &consumed,
                               &overflow, &left, &right, &center);
      Serial.printf("PW_STICK_INPUT_PATH stable=%u gesture=%u desired=%u delivered=%u queued=%u wait=%u emitted=%u consumed=%u overflow=%u left=%u right=%u center=%u\n",
                    stable, gesture, desired, delivered, queued, wait_release,
                    emitted, consumed, overflow, left, right, center);
    }
    if (command == 's' && !StickForegroundIsIr()) {
      unsigned active, speaker_ready, codec, mode, compare, begins;
      unsigned begin_errors, power_errors, tones, tone_errors, playing;
      StickSoundDiagnostic(&active, &speaker_ready, &codec, &mode, &compare,
                           &begins, &begin_errors, &power_errors, &tones,
                           &tone_errors, &playing);
      Serial.printf("PW_STICK_SOUND active=%u ready=%u codec=%u mode=%u compare=%u begins=%u begin_errors=%u power_errors=%u tones=%u tone_errors=%u playing=%u\n",
                    active, speaker_ready, codec, mode, compare, begins,
                    begin_errors, power_errors, tones, tone_errors, playing);
      unsigned measured, under20, under50, shortest;
      StickSoundTimingDiagnostic(&measured, &under20, &under50, &shortest);
      Serial.printf("PW_STICK_SOUND_TIMING measured=%u under20=%u under50=%u shortest_us=%u\n",
                    measured, under20, under50, shortest);
    }
    if (command == 'b' && !StickForegroundIsIr()) {
      auto &power = StickBoardPower();
      Serial.printf("PW_STICK_POWER battery_mv=%u vbus_mv=%u source=%u "
                    "ext_5v=%u inactive=%u sample_period_us=%llu\n",
                    unsigned(StickBatteryMillivolts()),
                    unsigned(power.getVBUSVoltage()),
                    unsigned(power.getPowerSource()),
                    unsigned(power.getExtOutput()),
                    unsigned(StickForegroundIsInactive()),
                    (unsigned long long)sample_period_us());
      uint64_t count, sleep_us, timer_wakes, gpio_wakes, errors;
      int last_sleep_error;
      unsigned sleep_gpio, sleep_duration;
      StickSleepDiagnostic(&count, &sleep_us, &timer_wakes, &gpio_wakes,
                           &errors, &last_sleep_error, &sleep_gpio,
                           &sleep_duration);
      Serial.printf("PW_STICK_SLEEP count=%llu sleep_us=%llu timer=%llu "
                    "gpio=%llu errors=%llu last_error=%d last_gpio=%u "
                    "duration=%u\n",
                    (unsigned long long)count,
                    (unsigned long long)sleep_us,
                    (unsigned long long)timer_wakes,
                    (unsigned long long)gpio_wakes,
                    (unsigned long long)errors,
                    last_sleep_error,
                    sleep_gpio, sleep_duration);
      unsigned accel_reads, accel_successes, accel_failures;
      StickBoardAccelDiagnostic(&accel_reads, &accel_successes,
                                &accel_failures);
      Serial.printf("PW_STICK_ACCEL reads=%u successes=%u failures=%u\n",
                    accel_reads, accel_successes, accel_failures);
    }
    if (command == 'o' && !StickForegroundIsIr()) {
      const u16 millivolts = StickBatteryMillivolts();
      Serial.printf("PW_STICK_DEVICE_MENU open=%u row=%u profile=%u orientation=%u chord_ms=%u battery_mv=%u battery_pct=%d usb=%u\n",
                    device_menu_open, device_menu_row,
                    StickInputProfile(), StickInputOrientation(),
                    80 + StickInputChordWindowIndex() * 40,
                    unsigned(millivolts), battery_percent_estimate(millivolts),
                    unsigned(StickBoardPower().getVBUSVoltage() >= 4000));
    }
    if (command == 't' && !StickForegroundIsIr()) {
      const uint64_t end = uint64_t(esp_timer_get_time());
      unsigned view, updates, frames, seconds, flags, idle, selection, pressed;
      StickForegroundUiDiagnostic(&view, &updates, &frames, &seconds,
                                  &flags, &idle, &selection, &pressed);
      Serial.printf("PW_STICK_TIMING span_us=%llu main=%llu main_us=%llu beep=%llu beep_us=%llu input=%llu input_us=%llu present_us=%llu lag_us=%lld\n",
                    (unsigned long long)(end - timing_started_us),
                    (unsigned long long)main_ticks,
                    (unsigned long long)main_us,
                    (unsigned long long)beep_ticks,
                    (unsigned long long)beep_us,
                    (unsigned long long)input_polls,
                    (unsigned long long)input_us,
                    (unsigned long long)present_us,
                    (long long)(int64_t(end) - int64_t(next_sample_us)));
      Serial.printf("PW_STICK_TIMING_EXTRA sound_us=%llu sound_max_us=%llu loop_gap_max_us=%llu\n",
                    (unsigned long long)sound_us,
                    (unsigned long long)sound_max_us,
                    (unsigned long long)loop_gap_max_us);
      Serial.printf("PW_STICK_UI view=%u updates=%u frames=%u seconds=%u flags=%02x idle=%u selection=%u pressed=%u\n",
                    view, updates, frames, seconds, flags, idle, selection,
                    pressed);
      timing_started_us = end;
      main_ticks = beep_ticks = input_polls = 0;
      main_us = beep_us = input_us = present_us = 0;
      sound_us = sound_max_us = loop_gap_max_us = 0;
    }
    if (command >= '1' && command <= '3' && !StickForegroundIsIr()) {
      const u8 button = command == '1' ? 4 : command == '2' ? 2 : 8;
      StickInputBenchInject(button);
      Serial.printf("PW_STICK_BUTTON_INJECTED value=%u\n", button);
    }
    if (command == 'z' && !StickForegroundIsIr()) {
      StickForegroundBenchSleep();
      present_game_if_changed(true);
      Serial.println("PW_STICK_BENCH_SLEEP");
    }
    if (command == 'Y' && !StickForegroundIsIr()) {
      usb_sleep_trial_end_us = uint64_t(esp_timer_get_time()) + 5000000;
      Serial.println("PW_STICK_USB_SLEEP_TRIAL duration_ms=5000");
    }
    if (command == 'n' && !StickForegroundIsIr()) {
      StickForegroundBenchInactive();
      present_game_if_changed(true);
      Serial.println("PW_STICK_BENCH_INACTIVE");
    }
    if (command == 'w' && !StickForegroundIsIr()) {
      StickInputBenchHold(2, 10);
      Serial.println("PW_STICK_BENCH_CENTER_HOLD");
    }
    if (command == 'v' && !StickForegroundIsIr()) {
      StickInputBenchMainHold(650);
      Serial.println("PW_STICK_BENCH_MAIN_HOLD");
    }
    if (command == 'l' && !StickForegroundIsIr()) {
      StickInputBenchPowerEvent();
      Serial.println("PW_STICK_BENCH_POWER_EVENT");
    }
    if (command == 'x' && !StickForegroundIsIr()) {
      const bool erased = StickEepromBenchErase();
      Serial.println(erased ? "PW_STICK_BENCH_ERASED" :
                     "PW_STICK_BENCH_ERASE_FAILED");
      Serial.flush();
      if (erased) ESP.restart();
    }
  }
#endif
  if (now >= next_quarter_us) {
    next_quarter_us += 250000;
    if (!StickForegroundIsInactive()) StickForegroundQuarterSecond();
  }
  if (now >= next_second_us) {
    next_second_us += 1000000;
    StickForegroundSecond();
  }

  if (device_menu_open && now - last_battery_draw_us >= 5000000)
    draw_battery_readout();

  if (StickForegroundIsIr()) {
    previous_ir = true;
    StickForegroundRun();
    if (StickForegroundIsIr())
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    return;
  }
  if (previous_ir) {
    previous_ir = false;
    // IR owns the foreground for seconds at a time. Those elapsed samples
    // cannot be replayed as a burst of MainTick calls after the handoff.
    next_sample_us = now + sample_period_us();
    next_beep_us = now + 8000;
#ifdef PW_STICK_BENCH_CONTROL
    if (StickIrBenchTraceEnabled())
      Serial.printf("PW_STICK_IR_DONE result=%u\n", StickForegroundIrResult());
    StickIrBenchTrace(0);
#endif
  }

#ifdef PW_STICK_BENCH_CONTROL
  const uint64_t sound_begin = uint64_t(esp_timer_get_time());
#endif
  StickSoundService();
#ifdef PW_STICK_BENCH_CONTROL
  const uint64_t sound_elapsed = uint64_t(esp_timer_get_time()) - sound_begin;
  sound_us += sound_elapsed;
  if (sound_elapsed > sound_max_us) sound_max_us = sound_elapsed;
#endif

  // A foreground owner may have held the loop past several sample slots.
  // Resume at the current wall clock rather than replaying obsolete ticks.
  const uint64_t sample_period = sample_period_us();
  if (now > next_sample_us + sample_period) next_sample_us = now;
  if (now > next_input_us + 5000) next_input_us = now;
  if (now > next_beep_us + 8000) next_beep_us = now;
  if (now >= next_input_us) {
    next_input_us += 5000;
#ifdef PW_STICK_BENCH_CONTROL
    const uint64_t begin = uint64_t(esp_timer_get_time());
#endif
    StickInputPoll(millis());
    // The H8's center IRQ resumes its fast sampling timer immediately. On
    // this board the comfort chord is resolved by polling, so hold that
    // cadence while a physical or queued gesture needs native input scans.
    if (sample_period_us() < sample_period &&
        next_sample_us > now + sample_period_us())
      next_sample_us = now + sample_period_us();
#ifdef PW_STICK_BENCH_CONTROL
    ++input_polls;
    input_us += uint64_t(esp_timer_get_time()) - begin;
#endif
    if (!device_menu_open && StickMenuRequested()) {
      device_menu_open = true;
      device_menu_row = 0;
      StickInputMenuMode(1);
      draw_device_menu();
    }
    if (device_menu_open) {
      const u8 buttons = StickInputTakeMenuButtons();
      if (buttons & 4u) {
        device_menu_open = false;
        StickInputMenuMode(0);
        StickForegroundDeviceMenuClosed();
        // The game occupies only the centered 192x128 region. Erase the
        // settings text in the border before returning to that framebuffer.
        StickBoardScreen()->fillScreen(TFT_BLACK);
        present_game_if_changed(true);
      } else if (buttons & 1u) {
        device_menu_row = (device_menu_row + 1u) % 4u;
        draw_device_menu();
      } else if (buttons & 2u) {
        const u8 profile = StickInputProfile();
        const u8 orientation = StickInputOrientation();
        const u8 chord = StickInputChordWindowIndex();
        const u8 next_profile = device_menu_row == 0 ? profile ^ 1u : profile;
        const u8 next_orientation = device_menu_row == 1 ? orientation ^ 1u : orientation;
        const u8 next_chord = device_menu_row == 2 ? (chord + 1u) % 3u : chord;
        if (device_menu_row == 3) {
          StickSoundTestTone();
        } else if (StickInputConfigure(next_profile, next_orientation,
                                       next_chord)) {
          if (next_orientation != orientation)
            StickDisplayPanelSetOrientation(next_orientation);
          draw_device_menu();
        }
      }
    }
  }
  if (StickForegroundIsMain() && now >= next_sample_us) {
    next_sample_us += sample_period;
#ifdef PW_STICK_BENCH_CONTROL
    const uint64_t begin = uint64_t(esp_timer_get_time());
#endif
    StickForegroundRun();
    if (sample_period_us() != sample_period)
      next_sample_us = now + sample_period_us();
#ifdef PW_STICK_BENCH_CONTROL
    ++main_ticks;
    main_us += uint64_t(esp_timer_get_time()) - begin;
#endif
    if (!StickForegroundIsIr() && !device_menu_open) {
#ifdef PW_STICK_BENCH_CONTROL
      const uint64_t present_begin = uint64_t(esp_timer_get_time());
#endif
      present_game_if_changed();
#ifdef PW_STICK_BENCH_CONTROL
      present_us += uint64_t(esp_timer_get_time()) - present_begin;
#endif
    }
    return;
  }
  if (StickForegroundIsBeep() && now >= next_beep_us) {
    next_beep_us += 8000;
#ifdef PW_STICK_BENCH_CONTROL
    const uint64_t begin = uint64_t(esp_timer_get_time());
#endif
    StickForegroundRun();
#ifdef PW_STICK_BENCH_CONTROL
    ++beep_ticks;
    beep_us += uint64_t(esp_timer_get_time()) - begin;
    const uint64_t present_begin = uint64_t(esp_timer_get_time());
#endif
    if (!device_menu_open) present_game_if_changed();
    if (StickForegroundIsMain()) next_sample_us = now + sample_period_us();
#ifdef PW_STICK_BENCH_CONTROL
    present_us += uint64_t(esp_timer_get_time()) - present_begin;
#endif
    return;
  }
  if (StickForegroundIsMain() && !device_menu_open &&
      !StickDisplayIsPowered() && !StickInputWakeScanActive() &&
      !Serial.available()) {
    // USB Serial/JTAG disconnects during ESP32-S3 light sleep. Keep the
    // charging/debug connection usable; the bench command Y runs a bounded
    // five-second real-sleep trial to verify the same path on hardware.
    if (now >= next_vbus_check_us) {
      usb_present = StickBoardPower().getVBUSVoltage() >= 4000;
      next_vbus_check_us = now + 1000000;
    }
    const bool usb_sleep_allowed = !usb_present
#ifdef PW_STICK_BENCH_CONTROL
        || uint64_t(esp_timer_get_time()) < usb_sleep_trial_end_us
#endif
        ;
    if (usb_sleep_allowed) {
      uint64_t deadline = next_sample_us;
      if (next_second_us < deadline) deadline = next_second_us;
      if (!StickForegroundIsInactive() && next_quarter_us < deadline)
        deadline = next_quarter_us;
      if (StickSleepUntil(deadline)) {
        // A switch can wake the chip between scheduled 5 ms polls. Poll it in
        // the next loop before returning to sleep or delivering a native scan.
        next_input_us = uint64_t(esp_timer_get_time());
        return;
      }
    }
  }
  delay(1);
}
