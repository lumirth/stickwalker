#include "board_diagnostics.h"

#ifdef PW_STICK_BENCH_CONTROL
#include "board_runtime.h"
#include "board_settings.h"
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
#include "backlight.h"
#include "native_runtime.h"
#include "diode_status.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <esp_timer.h>
#include <esp_attr.h>
#include <driver/rtc_io.h>
#include <soc/rtc_io_reg.h>
#include <driver/gpio.h>
#include <soc/gpio_periph.h>
#include <soc/io_mux_reg.h>

extern "C" void StickSoundBenchCapture(bool microphone, bool calibration, bool muted);
extern "C" void StickPortTrace(const char *message) {
  if (StickIrBenchTraceEnabled()) Serial.println(message);
}

namespace {
unsigned sample_bclk_edges() {
  // Observe the existing I2S output pad without taking over its matrix route.
  const uint32_t saved = REG_READ(GPIO_PIN_MUX_REG[17]);
  gpio_input_enable(GPIO_NUM_17);
  unsigned edges = 0;
  int previous = gpio_get_level(GPIO_NUM_17);
  const int64_t end = esp_timer_get_time() + 400;
  while (esp_timer_get_time() < end) {
    const int level = gpio_get_level(GPIO_NUM_17);
    edges += level != previous;
    previous = level;
  }
  REG_WRITE(GPIO_PIN_MUX_REG[17], saved);
  return edges;
}

bool passive_capture = false;
uint64_t passive_deadline_us = 0;
bool receive_trial = false;
uint64_t receive_deadline_us = 0;
uint64_t timing_started_us = 0;
uint64_t main_ticks = 0, beep_ticks = 0, input_polls = 0;
uint64_t main_us = 0, beep_us = 0, input_us = 0, present_us = 0;
uint64_t sound_us = 0, sound_max_us = 0, loop_gap_max_us = 0;
uint64_t last_loop_entry_us = 0;

bool previous_ir = false;

uint64_t battery_trial_end_us = 0;
bool battery_trial_armed = false;
RTC_NOINIT_ATTR struct {
  uint32_t magic;
  unsigned start_reads, end_reads, start_seconds, end_seconds;
  unsigned failures, completed;
  uint64_t start_us, end_us;
} retained_trial;
constexpr uint32_t kTrialMagic = 0x50575431;

void begin_battery_trial(uint64_t now) {
  unsigned successes, failures;
  StickBoardAccelDiagnostic(&retained_trial.start_reads, &successes, &failures);
  unsigned view, updates, frames, flags, idle, selection, pressed;
  StickForegroundUiDiagnostic(&view, &updates, &frames,
      &retained_trial.start_seconds, &flags, &idle, &selection, &pressed);
  retained_trial.magic = kTrialMagic;
  retained_trial.completed = 0;
  retained_trial.start_us = now;
  battery_trial_end_us = now + 5000000;
  battery_trial_armed = false;
}

}

void StickDiagnosticsInit(void) {
  if (retained_trial.magic == kTrialMagic)
    Serial.printf("PW_STICK_POWER_TRIAL_PREVIOUS completed=%u start_us=%llu "
                  "end_us=%llu start_reads=%u end_reads=%u failures=%u "
                  "start_seconds=%u end_seconds=%u\n",
                  retained_trial.completed,
                  (unsigned long long)retained_trial.start_us,
                  (unsigned long long)retained_trial.end_us,
                  retained_trial.start_reads, retained_trial.end_reads,
                  retained_trial.failures, retained_trial.start_seconds,
                  retained_trial.end_seconds);
  retained_trial = {};
  timing_started_us = uint64_t(esp_timer_get_time());
  Serial.println("PW_STICK_READY");
}

int StickDiagnosticsService(void) {
  const uint64_t now = uint64_t(esp_timer_get_time());
  if (StickForegroundIsIr()) previous_ir = true;
  else if (previous_ir) {
    previous_ir = false;
    if (StickIrBenchTraceEnabled())
      Serial.printf("PW_STICK_IR_DONE result=%u\n", StickForegroundIrResult());
    StickIrBenchTrace(0);
  }
  if (battery_trial_end_us && now >= battery_trial_end_us) {
    battery_trial_end_us = 0;
    // USB CDC may remain unavailable until the host resets/reconnects it.
    // Retain the completed trial across that CPU reset without save writes.
    unsigned successes;
    StickBoardAccelDiagnostic(&retained_trial.end_reads, &successes,
                              &retained_trial.failures);
    unsigned view, updates, frames, flags, idle, selection, pressed;
    StickForegroundUiDiagnostic(&view, &updates, &frames,
        &retained_trial.end_seconds, &flags, &idle, &selection, &pressed);
    retained_trial.end_us = now;
    retained_trial.completed = 1;
  }
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
    return 1;
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
    StickIrResumeReception();
    if (now >= receive_deadline_us || StickIrFailed()) {
      StickIrStop();
      receive_trial = false;
      Serial.println("PW_STICK_TRIAL_DONE");
      StickIrBenchTrace(0);
    } else {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
    }
    return 1;
  }
  while (Serial.available()) {
    const int command = Serial.read();
    if (command == 'u' && !StickForegroundIsIr()) {
      Serial.printf("PW_STICK_DIODE_BIAS_RESULT state=%u\n", StickDiodeBiasProbe());
    }
    if (command == 'h' && !StickForegroundIsIr()) {
      const uint64_t started = uint64_t(esp_timer_get_time());
      const unsigned state = StickDiodeProbe();
      Serial.printf("PW_STICK_DIODE_DISPLAY orientation=%u rotation=%u\n",
                    StickInputOrientation(), StickBoardScreen()->getRotation());
      Serial.printf("PW_STICK_DIODE_PROBE state=%u elapsed_us=%llu\n",
                    state, (unsigned long long)(esp_timer_get_time() - started));
    }
    if (command == 'm' && !StickForegroundIsIr()) {
      for (unsigned pin = 6; pin <= 8; ++pin) {
        const uint64_t started = uint64_t(esp_timer_get_time());
        const unsigned state = StickDiodeProbePin(pin);
        Serial.printf("PW_STICK_DIODE_PROBE pin=%u state=%u elapsed_us=%llu\n",
                      pin, state, (unsigned long long)(esp_timer_get_time() - started));
      }
    }
    if (command == 'c' && !StickForegroundIsIr()) {
      StickIrBenchTrace(1);
      StickForegroundRequestIr();
      Serial.println("PW_STICK_BENCH_CONNECT");
      if (!StickForegroundIsIr()) StickIrBenchTrace(0);
    }
    if (command == 'd' && !StickForegroundIsIr()) {
      // Capture a normal menu-initiated session without starting IR from the
      // host. The session completion path clears this one-shot trace flag.
      StickIrBenchTrace(1);
      Serial.println("PW_STICK_TRACE_ARMED");
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
        StickIrStop();
        StickIrPassiveDiagnostic(0);
        StickIrBenchTrace(0);
      }
      return passive_capture;
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
      if (!receive_trial) {
        StickIrStop();
        StickIrBenchTrace(0);
      }
      return receive_trial;
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
    if (command == 'j' && !StickForegroundIsIr()) {
      // Switch both buttons to RTC mode, then exercise the sleep-return GPIO
      // restoration while USB remains connected.
      const esp_err_t first = rtc_gpio_init(GPIO_NUM_11);
      const esp_err_t second = rtc_gpio_init(GPIO_NUM_12);
      const unsigned before =
          (REG_GET_BIT(RTC_IO_TOUCH_PAD11_REG, RTC_IO_TOUCH_PAD11_MUX_SEL) ? 1u : 0u) |
          (REG_GET_BIT(RTC_IO_TOUCH_PAD12_REG, RTC_IO_TOUCH_PAD12_MUX_SEL) ? 2u : 0u);
      const bool restored = StickSleepBegin();
      const unsigned after =
          (REG_GET_BIT(RTC_IO_TOUCH_PAD11_REG, RTC_IO_TOUCH_PAD11_MUX_SEL) ? 1u : 0u) |
          (REG_GET_BIT(RTC_IO_TOUCH_PAD12_REG, RTC_IO_TOUCH_PAD12_MUX_SEL) ? 2u : 0u);
      Serial.printf("PW_STICK_BUTTON_MUX first=%d second=%d before=%u restored=%u after=%u\n",
                    first, second, before, unsigned(restored), after);
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
      unsigned emitted, consumed, overflow, left, right, center, expired, cancelled;
      StickInputPathDiagnostic(&stable, &gesture, &desired, &delivered,
                               &queued, &wait_release, &emitted, &consumed,
                               &overflow, &left, &right, &center, &expired, &cancelled);
      Serial.printf("PW_STICK_INPUT_PATH stable=%u gesture=%u desired=%u delivered=%u queued=%u wait=%u emitted=%u consumed=%u overflow=%u left=%u right=%u center=%u expired=%u cancelled=%u\n",
                    stable, gesture, desired, delivered, queued, wait_release,
                    emitted, consumed, overflow, left, right, center, expired, cancelled);
    }
    if ((command == 'K' || command == 'U' || command == 'Q' || command == 'O') && !StickForegroundIsIr()) {
      StickSoundBenchCapture(command != 'U', command == 'Q', command == 'O');
      StickNativeService();
    }
    if (command == 'H' && !StickForegroundIsIr()) {
      const int started = StickForegroundBenchMoveScore();
      const unsigned early = sample_bclk_edges();
      delay(50);
      const unsigned late = sample_bclk_edges();
      StickNativeService();
      Serial.printf("PW_STICK_CODEC_CLOCK started=%u early_edges=%u late_edges=%u registers=",
                    started, early, late);
      for (const unsigned reg : {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                                0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x10,0x11,0x12,
                                0x13,0x31,0x32,0x37,0x44}) {
        uint8_t value = 0;
        const bool ok = m5::In_I2C.readRegister(0x18, reg, &value, 1, 100000);
        Serial.printf("%02x:%02x:%u,", reg, value, ok);
      }
      Serial.println();
    }
    if (command == 'G' && !StickForegroundIsIr()) {
      // Delay score servicing to reproduce foreground rendering stalls.
      const int started = StickForegroundBenchMoveScore();
      if (started) delay(80);
      StickNativeService();
      Serial.printf("PW_STICK_SOUND_STALL started=%u stall_ms=80\n", started);
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
      uint8_t gpio_out = 0, gpio_in = 0, gpio_mode = 0;
      uint8_t gpio_func = 0, pwr_cfg = 0, codec_reg = 0;
      const bool pmic_ok =
          m5::In_I2C.readRegister(0x6e, 0x11, &gpio_out, 1, 100000) &&
          m5::In_I2C.readRegister(0x6e, 0x12, &gpio_in, 1, 100000) &&
          m5::In_I2C.readRegister(0x6e, 0x10, &gpio_mode, 1, 100000) &&
          m5::In_I2C.readRegister(0x6e, 0x16, &gpio_func, 1, 100000) &&
          m5::In_I2C.readRegister(0x6e, 0x06, &pwr_cfg, 1, 100000);
      const bool codec_ok = m5::In_I2C.readRegister(
          0x18, 0x00, &codec_reg, 1, 100000);
      unsigned m_edges, r_edges, l_edges, l_events, pmic_errors, raw, logical, power_ready;
      StickInputDiagnostic(&m_edges, &r_edges, &l_edges, &l_events, &pmic_errors,
                           &raw, &logical, &power_ready);
      Serial.printf("PW_STICK_SOUND_HW pmic_ok=%u gpio_out=%02x gpio_in=%02x gpio_mode=%02x gpio_func=%02x pwr_cfg=%02x codec_ok=%u codec_00=%02x input_raw=%u power_edges=%u\n",
                    unsigned(pmic_ok), gpio_out, gpio_in, gpio_mode,
                    gpio_func, pwr_cfg, unsigned(codec_ok), codec_reg,
                    raw, l_edges);
    }
    if (command == 'a' && !StickForegroundIsIr())
      Serial.printf("PW_STICK_SOUND_BENCH_TONE started=%u duration_ms=8000\n",
                    unsigned(StickSoundBenchTone()));
    if (command == 'b' && !StickForegroundIsIr()) {
      auto &power = StickBoardPower();
      Serial.printf("PW_STICK_POWER battery_mv=%u vbus_mv=%u source=%u "
                    "ext_5v=%u inactive=%u sample_period_us=%llu\n",
                    unsigned(StickBatteryMillivolts()),
                    unsigned(power.getVBUSVoltage()),
                    unsigned(power.getPowerSource()),
                    unsigned(power.getExtOutput()),
                    unsigned(StickForegroundIsInactive()),
                    (unsigned long long)(StickForegroundIsInactive() ? UINT64_C(1000000) : UINT64_C(62500)));
      uint64_t count, sleep_us, timer_wakes, gpio_wakes, errors, rejections;
      int last_sleep_error;
      unsigned sleep_gpio, sleep_duration;
      StickSleepDiagnostic(&count, &sleep_us, &timer_wakes, &gpio_wakes,
                           &errors, &rejections, &last_sleep_error, &sleep_gpio,
                           &sleep_duration);
      Serial.printf("PW_STICK_SLEEP count=%llu sleep_us=%llu timer=%llu "
                    "gpio=%llu errors=%llu last_error=%d last_gpio=%u "
                    "duration=%u rejections=%llu\n",
                    (unsigned long long)count,
                    (unsigned long long)sleep_us,
                    (unsigned long long)timer_wakes,
                    (unsigned long long)gpio_wakes,
                    (unsigned long long)errors,
                    last_sleep_error,
                    sleep_gpio, sleep_duration,
                    (unsigned long long)rejections);
      unsigned accel_reads, accel_successes, accel_failures;
      StickBoardAccelDiagnostic(&accel_reads, &accel_successes,
                                &accel_failures);
      Serial.printf("PW_STICK_ACCEL reads=%u successes=%u failures=%u\n",
                    accel_reads, accel_successes, accel_failures);
    }
    if (command == 'F' && !StickForegroundIsIr() &&
        StickDisplayPanelIsReady()) {
      // Exercise cold panel wake and finish with the current game image
      // displayed.
      auto *screen = StickBoardScreen();
      auto *panel = screen->getPanel();
      screen->startWrite(); screen->writeCommand(0x28); screen->endWrite();
      const unsigned off = panel->readCommand(0x0a, 0, 1);
      screen->writeCommand(0x29);  // Reproduce the former unselected write.
      panel->waitDMA();
      const unsigned unselected = panel->readCommand(0x0a, 0, 1);
      screen->startWrite(); screen->writeCommand(0x29); screen->endWrite();
      const unsigned selected = panel->readCommand(0x0a, 0, 1);
      Serial.printf("PW_STICK_LCD_CS off=%02x unselected=%02x selected=%02x\n",
                    off, unselected, selected);
    }
    if (command == 'D' && !StickForegroundIsIr()) {
      auto *screen = StickBoardScreen();
      if (!screen || !StickDisplayPanelIsReady()) {
        Serial.println("PW_STICK_LCD_REG ready=0");
      } else {
        auto *panel = screen->getPanel();
        Serial.printf("PW_STICK_LCD_REG ready=1 power=%02x madctl=%02x pixel=%02x\n",
                      unsigned(panel->readCommand(0x0a, 0, 1)),
                      unsigned(panel->readCommand(0x0b, 0, 1)),
                      unsigned(panel->readCommand(0x0c, 0, 1)));
      }
    }
    if (command == 'P' && !StickForegroundIsIr()) {
      uint8_t gpio_out = 0, pwr_cfg = 0, accel_conf = 0;
      uint8_t imu_power = 0, imu_power_conf = 0;
      const bool pmic_ok =
          m5::In_I2C.readRegister(0x6e, 0x11, &gpio_out, 1, 100000) &&
          m5::In_I2C.readRegister(0x6e, 0x06, &pwr_cfg, 1, 100000);
      const bool imu_ok =
          m5::In_I2C.readRegister(0x68, 0x40, &accel_conf, 1, 100000) &&
          m5::In_I2C.readRegister(0x68, 0x7d, &imu_power, 1, 100000) &&
          m5::In_I2C.readRegister(0x68, 0x7c, &imu_power_conf, 1, 100000);
      Serial.printf("PW_STICK_POWER_HW cpu_mhz=%u panel_awake=%u "
                    "pwm_ready=%u sound_busy=%u pmic_ok=%u gpio_out=%02x "
                    "pwr_cfg=%02x imu_ok=%u accel_conf=%02x imu_power=%02x "
                    "imu_power_conf=%02x\n",
                    unsigned(getCpuFrequencyMhz()), StickDisplayPanelIsReady(),
                    StickBacklightSleepReady(), StickSoundIsBusy(),
                    unsigned(pmic_ok), gpio_out, pwr_cfg, unsigned(imu_ok),
                    accel_conf, imu_power, imu_power_conf);
    }
    if (command == 'o' && !StickForegroundIsIr()) {
      const u16 millivolts = StickBatteryMillivolts();
      Serial.printf("PW_STICK_DEVICE_MENU open=%u row=%u profile=%u orientation=%u chord_ms=%u battery_mv=%u battery_pct=%d usb=%u layout=%u dark=%u\n",
                    StickSettingsIsOpen(), StickSettingsSelection(),
                    StickInputProfile(), StickInputOrientation(),
                    80 + StickInputChordWindowIndex() * 40,
                    unsigned(millivolts), StickBatteryPercentEstimate(millivolts),
                    unsigned(StickBoardPower().getVBUSVoltage() >= 4000),
                    StickInputLayout(), StickDisplayIsDark());
    }
    if (command == 't' && !StickForegroundIsIr()) {
      const uint64_t end = uint64_t(esp_timer_get_time());
      const uint64_t deadline = StickNativeNextDeadline();
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
                    (long long)(deadline == UINT64_MAX ? 0 : int64_t(end) - int64_t(deadline)));
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
      StickPresentGame(1);
      Serial.println("PW_STICK_BENCH_SLEEP");
    }
    if (command == 'Y' && !StickForegroundIsIr()) {
      battery_trial_armed = true;
      retained_trial = {};
      Serial.println("PW_STICK_BATTERY_TRIAL_ARMED duration_ms=5000");
    }
    if (command == 'n' && !StickForegroundIsIr()) {
      StickForegroundBenchInactive();
      StickPresentGame(1);
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
    if (command == 'V' && !StickForegroundIsIr()) {
      StickInputBenchMainHold(40);  // Abandon preparation before wake acceptance.
      Serial.println("PW_STICK_BENCH_MAIN_TAP");
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
  if (StickForegroundIsIr()) previous_ir = true;
  return 0;
}

void StickDiagnosticsForeground(unsigned task, uint64_t elapsed) {
  if (task == 1) {
    ++main_ticks;
    main_us += elapsed;
  } else if (task == 2) {
    ++beep_ticks;
    beep_us += elapsed;
  }
}

void StickDiagnosticsInput(uint64_t elapsed) {
  ++input_polls;
  input_us += elapsed;
}

void StickDiagnosticsSound(uint64_t elapsed) {
  sound_us += elapsed;
  if (elapsed > sound_max_us) sound_max_us = elapsed;
}

void StickDiagnosticsPresent(uint64_t elapsed) { present_us += elapsed; }

void StickDiagnosticsBeforeSleep(int usb_present) {
  if (!usb_present && battery_trial_armed)
    begin_battery_trial(uint64_t(esp_timer_get_time()));
}
#endif
