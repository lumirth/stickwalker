#!/usr/bin/env python3
"""Trace real production adapter paths on a host, without a device or save data.

This is a lifecycle probe, not a current or battery simulator. Hardware and
native foreground work are stubbed; the production runtime, display adapter,
sound adapter, and sleep scheduler are compiled unchanged. The speaker stub
records begin/stop/end calls. Regression cases also exercise power ownership,
wake restoration, dirty display transfers, PWM setup, and fault recovery.
See the separately audited M5Unified implementation
for the electrical consequences of those calls.
"""

from pathlib import Path
import json
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
HEADERS = {
    "Preferences.h": r"""
#pragma once
#include <cstdint>
#include <map>
#include <string>
inline std::map<std::string, uint8_t> preference_values;
inline bool preference_write_fault_once = false;
class Preferences {
  std::string space;
 public:
  bool begin(const char *name, bool) { space=name; return true; }
  void end() {}
  uint8_t getUChar(const char *key, uint8_t fallback) {
    auto it=preference_values.find(space+"/"+key);
    return it==preference_values.end() ? fallback : it->second;
  }
  size_t putUChar(const char *key, uint8_t value) {
    if (preference_write_fault_once) { preference_write_fault_once=false; return 0; }
    preference_values[space+"/"+key]=value; return 1;
  }
};
""",
    "Arduino.h": r"""
#pragma once
#include <cstdint>
extern uint64_t clock_us;
extern int serial_bytes;
struct SerialStub {
  void begin(int) {}
  int available() { return serial_bytes; }
  int read() { return serial_bytes ? (--serial_bytes, 0) : -1; }
  template<class... T> void printf(const char *, T...) {}
};
extern SerialStub Serial;
extern unsigned cpu_mhz;
inline bool setCpuFrequencyMhz(unsigned mhz) { cpu_mhz = mhz; return true; }
inline void delay(unsigned ms) { clock_us += ms * 1000; }
inline unsigned long millis() { return clock_us / 1000; }
inline int digitalRead(int) { return 1; }
inline unsigned ulTaskNotifyTake(int, unsigned ticks) {
  clock_us += ticks * 1000; return 0;
}
#define LOW 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
""",
    "M5Unified.h": r"""
#pragma once
#include <cstdint>
#include "backlight.h"
#include <cassert>
namespace lgfx {
struct LGFX_Device {
  unsigned transaction_depth = 0, unselected_commands = 0;
  bool output_enabled = true, asleep = false;
  unsigned brightness = 68;
  unsigned sleep_calls = 0, wake_calls = 0, init_calls = 0;
  unsigned pushes = 0, last_x = 0, last_y = 0, last_w = 0, last_h = 0;
  uint16_t first_pixel = 0;
  LGFX_Device *getPanel() { return this; }
  void initBus() {}
  void releaseBus() {}
  bool init() { ++init_calls; output_enabled = false; asleep = true; return StickBacklightInit(0); }
  bool getInvert() { return false; }
  void invertDisplay(bool) {}
  void setColorDepth(unsigned) {}
  void startWrite() { ++transaction_depth; }
  void endWrite() { assert(transaction_depth); --transaction_depth; }
  void writeCommand(unsigned command) {
    // The installed Panel_Device forwards commands without asserting CS.
    // A cold ST7789 ignores those bytes unless its caller owns a transaction.
    if (!transaction_depth) { ++unselected_commands; return; }
    if (command == 0x29) output_enabled = true;
  }
  void setBrightness(unsigned value) { brightness = value; StickBacklightSet(value); }
  void setSleep(bool on) { sleep_calls += on; }
  void sleep() { ++sleep_calls; asleep = true; }
  void wakeup() { ++wake_calls; asleep = false; }
  void setSwapBytes(bool) {}
  void setRotation(unsigned) {}
  void fillScreen(unsigned) {}
  template<class... T> void fillRect(T...) {}
  template<class... T> void setTextColor(T...) {}
  template<class... T> void setCursor(T...) {}
  template<class... T> void setTextSize(T...) {}
  template<class... T> void print(T...) {}
  template<class... T> void printf(T...) {}
  template<class... T> void drawFastHLine(T...) {}
  void pushImage(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t *p) {
    assert(output_enabled && !asleep && "visible wake must send selected DISPON before presenting");
    ++pushes; last_x=x; last_y=y; last_w=w; last_h=h; first_pixel=p[0];
  }
};
}
namespace m5 {
struct M5PM1_Class {
  enum gpio_t { gpio0, gpio1, gpio2, gpio3, gpio4 };
  bool levels[5] = {};
  bool setGPIOOutput(gpio_t pin, bool on) { levels[pin] = on; return true; }
  bool setExtOutput(bool) { return true; }
  unsigned getVBUSVoltage() { return 0; }
};
extern bool codec_fault_once;
struct I2CStub {
  unsigned writes = 0;
  uint8_t codec[256] = {};
  bool readRegister(unsigned address, unsigned reg, uint8_t *value, unsigned, unsigned) {
    *value = address == 0x18 ? codec[reg] : 0; return true;
  }
  bool writeRegister8(unsigned address, unsigned reg, uint8_t value, unsigned) {
    if (address == 0x18) {
      ++writes;
      if (codec_fault_once && reg == 0x0d) { codec_fault_once=false; return false; }
      codec[reg] = value;
    }
    return true;
  }
};
extern I2CStub In_I2C;
}
extern bool speaker_begin_fault_once;
extern bool speaker_tone_fault_once;
struct SpeakerStub {
  struct Config {
    int pin_mck, pin_bck, pin_ws, pin_data_out, i2s_port, magnification;
    int sample_rate, dma_buf_len, dma_buf_count, task_pinned_core;
    bool stereo, buzzer, use_dac;
    int dac_zero_level;
  } cfg{};
  unsigned begin_calls = 0, end_calls = 0, stop_calls = 0;
  Config config() { return cfg; }
  void config(Config value) { cfg = value; }
  bool begin() {
    ++begin_calls;
    if (speaker_begin_fault_once) { speaker_begin_fault_once=false; return false; }
    return true;
  }
  void end() { ++end_calls; }
  void stop() { ++stop_calls; }
  void setVolume(unsigned) {}
  template<class... T> bool tone(T...) {
    if (speaker_tone_fault_once) { speaker_tone_fault_once=false; return false; }
    return true;
  }
};
struct M5Stub { SpeakerStub Speaker; };
extern M5Stub M5;
#define I2S_NUM_0 0
#define TFT_BLACK 0
#define TFT_WHITE 65535
""",
    "esp_timer.h": r"""
#pragma once
#include <cstdint>
extern uint64_t clock_us;
inline int64_t esp_timer_get_time() { return clock_us; }
""",
    "esp_heap_caps.h": r"""
#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
inline void *heap_caps_malloc(size_t size, unsigned) { return malloc(size); }
""",
    "driver/rtc_io.h": r"""
#pragma once
#include <initializer_list>
extern bool sleep_init_failed;
extern unsigned rtc_owned;
enum gpio_num_t { GPIO_NUM_11 = 11, GPIO_NUM_12 = 12 };
extern unsigned setup_attempts;
inline int rtc_gpio_pullup_en(gpio_num_t pin) {
  setup_attempts += pin == GPIO_NUM_11;
  return sleep_init_failed ? -1 : 0;
}
inline int rtc_gpio_pulldown_dis(gpio_num_t) { return 0; }
inline int rtc_gpio_hold_dis(gpio_num_t) { return 0; }
inline int rtc_gpio_deinit(gpio_num_t pin) { rtc_owned &= ~(1u << (unsigned(pin) - 11)); return 0; }
""",
    "esp_sleep.h": r"""
#pragma once
#include <cstdint>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_SLEEP_REJECT 259
#define ESP_ERR_SLEEP_TOO_SHORT_SLEEP_DURATION 258
#define ESP_PD_DOMAIN_RTC_PERIPH 0
#define ESP_PD_OPTION_ON 1
#define ESP_EXT1_WAKEUP_ANY_LOW 0
#define ESP_SLEEP_WAKEUP_TIMER 0
#define ESP_SLEEP_WAKEUP_EXT1 1
extern uint64_t clock_us, timer_duration_us;
extern unsigned completed_sleeps;
extern bool sleep_start_failed;
extern unsigned rtc_owned;
extern int sleep_reject_once;
inline int esp_sleep_pd_config(int, int) { return 0; }
inline int esp_sleep_enable_ext1_wakeup_io(uint64_t, int) { return 0; }
inline int esp_sleep_enable_timer_wakeup(uint64_t duration) {
  timer_duration_us = duration; return 0;
}
inline int esp_light_sleep_start() {
  rtc_owned = 3; // EXT1 prepares the pads, including cancelled entries.
  if (sleep_start_failed) { sleep_start_failed = false; return -2; }
  if (sleep_reject_once) { int result=sleep_reject_once; sleep_reject_once=0; return result; }
  clock_us += timer_duration_us; ++completed_sleeps; return 0;
}
inline int esp_sleep_get_wakeup_cause() { return ESP_SLEEP_WAKEUP_TIMER; }
""",
}

HEADERS["driver/gpio.h"] = r"""
#pragma once
#include <driver/rtc_io.h>
#define ESP_OK 0
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ONLY 1
inline int gpio_set_direction(gpio_num_t, int) { return 0; }
inline int gpio_set_pull_mode(gpio_num_t, int) { return 0; }
"""
HEADERS["driver/ledc.h"] = r"""
#pragma once
#include <cassert>
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_TIMER_3 3
#define LEDC_CHANNEL_7 7
#define LEDC_TIMER_9_BIT 9
#define LEDC_USE_RC_FAST_CLK 1
#define LEDC_INTR_DISABLE 0
#define LEDC_SLEEP_MODE_KEEP_ALIVE 2
struct ledc_timer_config_t {
  int speed_mode, timer_num, duty_resolution, freq_hz, clk_cfg;
  bool deconfigure;
};
struct ledc_channel_config_t {
  int gpio_num, speed_mode, channel, timer_sel, intr_type;
  unsigned duty;
  int sleep_mode;
};
extern bool ledc_timer_active, ledc_update_fault_once;
inline int ledc_timer_config(const ledc_timer_config_t *p) {
  assert(p->deconfigure || p->clk_cfg == LEDC_USE_RC_FAST_CLK);
  ledc_timer_active = !p->deconfigure;
  return 0;
}
inline int ledc_channel_config(const ledc_channel_config_t *p) {
  assert(p->sleep_mode == LEDC_SLEEP_MODE_KEEP_ALIVE); return 0;
}
inline int ledc_set_duty(int, int, unsigned) { return 0; }
inline int ledc_update_duty(int, int) {
  if (ledc_update_fault_once) { ledc_update_fault_once=false; return -1; }
  return 0;
}
inline int ledc_stop(int, int, int) { return 0; }
inline int ledc_timer_pause(int, int) { return 0; }
"""

HARNESS = r"""
#include <Arduino.h>
#include <M5Unified.h>
#include "display_bus.h"
#include "sound_bridge.h"
#include "peripheral_power.h"
#include "backlight.h"
#include "display_panel.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <Preferences.h>
uint64_t clock_us = 100000, timer_duration_us = 0;
int serial_bytes = 0;
bool sleep_init_failed = false, sleep_start_failed = false;
int sleep_reject_once = 0;
unsigned setup_attempts = 0, beep_advances = 0, cpu_mhz = 240;
bool vbus_fault = false, vbus_fault_once = false;
bool pending_wake_scan = false;
unsigned rtc_owned = 0;
bool lit_case = false, moving_case = false, supply_fault_once = false;
bool supply_on_fault_once = false, speaker_begin_fault_once = false;
bool speaker_tone_fault_once = false;
bool ledc_timer_active = false, ledc_update_fault_once = false;
bool menu_requested = false;
u8 input_layout = 0, input_orientation = 0, input_chord = 0;
u8 menu_buttons = 0;
unsigned completed_sleeps = 0;
unsigned main_runs = 0;
SerialStub Serial;
M5Stub M5;
namespace m5 { I2CStub In_I2C; bool codec_fault_once=false; }
lgfx::LGFX_Device screen;
m5::M5PM1_Class power;
bool StickBoardBegin() {
  // The board starts with the display supply powered; compile the real
  // shared-ownership state machine above this physical write stub.
  power.levels[m5::M5PM1_Class::gpio2] = true;
  StickPeripheralPowerInit();
  StickBacklightInit(68);
  return true;
}
bool StickBoardPeripheralSupply(bool on) {
  if (on && supply_on_fault_once) { supply_on_fault_once=false; return false; }
  if (!on && supply_fault_once) { supply_fault_once=false; return false; }
  power.levels[m5::M5PM1_Class::gpio2] = on;
  if (!on) std::memset(m5::In_I2C.codec, 0, 256);
  return true;
}
void StickBoardDisplayReset(bool) {}
bool StickBoardDisplayInitRegisters() { return screen.init(); }
lgfx::LGFX_Device *StickBoardScreen() { return &screen; }
m5::M5PM1_Class &StickBoardPower() { return power; }
bool StickBoardVbusVoltage(uint16_t *mv) {
  if (vbus_fault) return false;
  if (vbus_fault_once) { vbus_fault_once=false; return false; }
  *mv=0; return true;
}
extern "C" {
void StickPortSetup();
void StickPortLoop();
void BeepAdvance() { ++beep_advances; }
void StickPortBoot() {
  StickSoundInit();
  IO.PDR1.BIT.B1 = 0;
  StickDisplayWrite(lit_case ? 0xe1 : 0xa9);
}
int StickEepromMount() { return 1; }
void StickEepromDefer(int) {}
u16 StickBatteryMillivolts() { return 4000; }
int StickForegroundIsIr() { return 0; }
int StickForegroundIsMain() { return 1; }
int StickForegroundIsBeep() { return 0; }
int StickForegroundIsInactive() { return !moving_case; }
unsigned StickForegroundUiFrame() { return 0; }
void StickForegroundQuarterSecond() {}
void StickForegroundSecond() {}
void StickForegroundRun() { ++main_runs; }
void StickForegroundDeviceMenuClosed() {
  IO.PDR1.BIT.B1=0; StickDisplayWrite(0xe1);
}
void StickInputPoll(unsigned long) {
  assert(!rtc_owned && "physical switches must return to digital GPIO before polling");
}
int StickInputWakeScanActive() { return pending_wake_scan; }
int StickMenuRequested() {
  bool requested=menu_requested; menu_requested=false; return requested;
}
void StickInputMenuMode(int) {}
u8 StickInputTakeMenuButtons() {
  u8 buttons=menu_buttons; menu_buttons=0; return buttons;
}
u8 StickInputProfile() { return input_layout >= 2; }
u8 StickInputLayout() { return input_layout; }
u8 StickInputOrientation() { return input_orientation; }
u8 StickInputChordWindowIndex() { return input_chord; }
int StickInputConfigure(u8 layout, u8 orientation, u8 chord) {
  input_layout=layout; input_orientation=orientation; input_chord=chord; return 1;
}
}
int main(int argc, char **argv) {
  const bool serial_case = argc > 1 && !strcmp(argv[1], "serial");
  const bool error_case = argc > 1 && !strcmp(argv[1], "init-error");
  const bool persistent_error = argc > 1 && !strcmp(argv[1], "persistent-error");
  lit_case = argc > 1 && !strcmp(argv[1], "lit");
  moving_case = argc > 1 && !strcmp(argv[1], "moving");
  sleep_start_failed = argc > 1 && !strcmp(argv[1], "sleep-error");
  sleep_reject_once = argc > 1 && !strcmp(argv[1], "sleep-reject") ? 259 :
                      argc > 1 && !strcmp(argv[1], "sleep-short") ? 258 : 0;
  sleep_init_failed = error_case || persistent_error;
  supply_fault_once = argc>1 && !strcmp(argv[1], "supply-error");
  m5::codec_fault_once = argc>1 && !strcmp(argv[1], "codec-error");
  vbus_fault = argc>1 && !strcmp(argv[1], "vbus-error");
  vbus_fault_once = argc>1 && !strcmp(argv[1], "vbus-transient");
  StickPortSetup();
  assert(cpu_mhz == 80);
  // A gesture can already be queued before the runtime polls again.
  pending_wake_scan = argc>1 && !strcmp(argv[1], "queued-wake");
  if (serial_case) serial_bytes = 1;
  const uint64_t end = clock_us + 1200000;
  unsigned guard = 0;
  while (clock_us < end && ++guard < 10000) {
    if (error_case && clock_us + 900000 >= end) sleep_init_failed = false;
    StickPortLoop();
  }
  if (guard == 10000) return 2;
  std::printf("{\"setup_attempts\":%u,\"sleeps\":%u,\"main_runs\":%u,\"serial_bytes\":%d,\"brightness\":%u,"
              "\"lcd_sleep_calls\":%u,\"l3b_on\":%s,",
              setup_attempts, completed_sleeps, main_runs, serial_bytes, screen.brightness,
              screen.sleep_calls, power.levels[2] ? "true" : "false");
  if (argc>1 && (!strcmp(argv[1], "supply-on-error") ||
                  !strcmp(argv[1], "speaker-error"))) {
    supply_on_fault_once = !strcmp(argv[1], "supply-on-error");
    speaker_begin_fault_once = !strcmp(argv[1], "speaker-error");
    StickSoundEnable(); StickSoundDisable();
    assert(!StickSoundIsBusy() && !power.levels[2] && !power.levels[3]);
  }
  const unsigned begins_at_start = M5.Speaker.begin_calls;
  const unsigned ends_at_start = M5.Speaker.end_calls;
  // The cold amplifier starts the native score clock after the verified
  // 65 ms warm-up. A second cue in the hold interval must reuse the output.
  StickSoundEnable();
  StickSoundPeriod(40, 2);
  const uint64_t tone_begin = clock_us;
  clock_us = tone_begin + 64999;
  StickSoundService();
  assert(beep_advances == 0);
  clock_us = tone_begin + 65000;
  StickSoundService();
  assert(beep_advances == 1);
  StickSoundDisable();
  clock_us += 200000;
  StickSoundService();
  assert(power.levels[3] && M5.Speaker.end_calls == ends_at_start);
  StickSoundEnable();
  assert(M5.Speaker.begin_calls == begins_at_start + 1);
  StickSoundService();
  assert(beep_advances > 1);
  StickSoundPeriod(40, 2);
  const unsigned writes_before_disable = m5::In_I2C.writes;
  StickSoundDisable();
  clock_us += 600000;
  StickSoundService();
  std::printf("\"amp_on_after_600ms\":%s,\"speaker_end_calls\":%u,"
              "\"codec_writes_during_shutdown\":%u,\"codec_reg_0d\":%u}",
              power.levels[3] ? "true" : "false", M5.Speaker.end_calls,
              m5::In_I2C.writes - writes_before_disable,
              m5::In_I2C.codec[0x0d]);
  // Restore the same native image after supply loss. Identical images do
  // not transfer; one changed pixel transfers its 2x2 scaled rectangle.
  IO.PDR1.BIT.B1 = 0;
  StickDisplayWrite(0xe1);
  // Restoration is asynchronous: no 200 ms block in an input/motion call.
  // lit case retains RAM; use an explicit sleep/supply cycle for this check.
  StickDisplayWrite(0xa9); StickDisplayPresent();
  clock_us += 800000; StickDisplayPowerService();
  StickDisplayWrite(0xe1);
  const uint64_t wake_begin = clock_us;
  assert(!StickDisplayPresent());
  assert(clock_us - wake_begin <= 2000);
  clock_us += 8000; StickDisplayPowerService();
  clock_us += 64000; StickDisplayPowerService();
  clock_us += 130000; StickDisplayPowerService();
  assert(StickDisplayPresent());
  assert(screen.transaction_depth == 0 && screen.unselected_commands == 0);
  unsigned pushes = screen.pushes;
  StickDisplayPresent();
  assert(screen.pushes == pushes);
  StickDisplayWrite(0x10); StickDisplayWrite(5); StickDisplayWrite(0xb0);
  IO.PDR1.BIT.B1 = 1;
  StickDisplayWrite(0x80); StickDisplayWrite(0);
  IO.PDR1.BIT.B1 = 0;
  StickDisplayPresent();
  assert(screen.pushes == pushes + 1);
  assert(screen.last_x == 34 && screen.last_y == 17);
  assert(screen.last_w == 2 && screen.last_h == 2);
  assert(screen.first_pixel == 0xad75);
  StickDisplayInvalidate();
  StickDisplayPresent();
  assert(screen.last_w == 192 && screen.last_h == 128);
  // Keep LCD power when sound ends; warm codec resume restores registers
  // while the LCD owner still holds the shared rail.
  StickSoundEnable();
  const unsigned before = m5::In_I2C.codec[0x0e];
  StickSoundDisable();
  clock_us += 600000;
  StickSoundService();
  assert(power.levels[2] && !power.levels[3]);
  assert(m5::In_I2C.codec[0x0d] == 0xfc);
  StickSoundEnable();
  assert(m5::In_I2C.codec[0x0e] == before);
  const unsigned begins = M5.Speaker.begin_calls;
  // LCD sleep must not cut off active sound, nor reinitialize virtual RAM.
  StickDisplayWrite(0xa9);
  StickDisplayPresent();
  assert(power.levels[2] && power.levels[3]);
  // Also wake while sound retains the shared rail: no reset/init occurs,
  // but the delayed IDMOFF/DISPON commands still need selected transactions.
  clock_us += 800000; StickDisplayPowerService();
  assert(!StickDisplayPanelIsReady() && power.levels[2]);
  const unsigned init_before_warm = screen.init_calls;
  StickDisplayWrite(0xe1);
  assert(!StickDisplayPresent());
  clock_us += 130000; StickDisplayPowerService();
  assert(StickDisplayPresent() && screen.init_calls == init_before_warm);
  assert(screen.transaction_depth == 0 && screen.unselected_commands == 0);
  StickDisplayWrite(0xa9); StickDisplayPresent();
  StickSoundDisable();
  clock_us += 800000;
  StickSoundService();
  StickDisplayPowerService();
  assert(!power.levels[2] && !power.levels[3]);
  assert(M5.Speaker.begin_calls == begins);
  StickDisplayWrite(0xe1);
  assert(!StickDisplayPresent());
  clock_us += 8000; StickDisplayPowerService();
  clock_us += 64000; StickDisplayPowerService();
  clock_us += 130000; StickDisplayPowerService();
  assert(StickDisplayPresent());
  assert(screen.last_w == 192 && screen.last_h == 128);
  uint8_t restored[96*64];
  StickDisplayFrame(restored, sizeof(restored));
  assert(restored[7*96+5] == 2);
  assert(StickBacklightSleepReady());
  // Exercise the real runtime overlay path, including a full cursor cycle.
  menu_requested=true;
  uint64_t until=clock_us+300000;
  while (clock_us<until) StickPortLoop();
  const unsigned overlay_pushes=screen.pushes;
  // Four layout choices cycle without touching rotation. Rotation changes do
  // not touch layout. Appearance changes persist, invalidate and repaint.
  for (unsigned i=0;i<4;++i) {
    menu_buttons=2;
    until=clock_us+150000;
    while (clock_us<until) StickPortLoop();
    assert(input_layout==(i+1)%4 && input_orientation==0);
  }
  menu_buttons=1; until=clock_us+150000;
  while (clock_us<until) StickPortLoop();
  menu_buttons=2; until=clock_us+150000;
  while (clock_us<until) StickPortLoop();
  assert(input_layout==0 && input_orientation==1);
  menu_buttons=1; until=clock_us+150000;
  while (clock_us<until) StickPortLoop();
  menu_buttons=2; until=clock_us+150000;
  while (clock_us<until) StickPortLoop();
  assert(!StickDisplayIsDark() && preference_values["pw-display/dark"]==0);
  preference_write_fault_once=true;
  assert(!StickDisplaySetDark(1) && !StickDisplayIsDark());
  for (unsigned i=0;i<10;++i) {
    menu_buttons=1;
    until=clock_us+150000;
    while (clock_us<until) StickPortLoop();
    assert(!menu_buttons);
  }
  menu_buttons=4;
  until=clock_us+150000;
  while (clock_us<until) StickPortLoop();
  assert(!menu_buttons && screen.pushes>overlay_pushes);
  assert(screen.last_w==192 && screen.last_h==128);
  assert(screen.first_pixel==0xffff && StickDisplayBackground()==0xffff &&
         StickDisplayForeground()==0);
  // An abandoned board overlay must relinquish the physical display without
  // waking a native screen whose own timeout has already expired.
  menu_requested=true;
  until=clock_us+300000;
  while (clock_us<until) StickPortLoop();
  IO.PDR1.BIT.B1=0; StickDisplayWrite(0xa9);
  clock_us += 91000000;
  StickPortLoop(); StickDisplayPowerService();
  assert(!StickDisplayIsPowered() && !power.levels[2]);
  // Failed operations must not turn idle shutdown into a permanent lease.
  assert(StickBacklightInit(0));
  ledc_update_fault_once = true;
  StickBacklightSet(40);
  assert(!StickBacklightSleepReady() && ledc_timer_active);
  StickBacklightSuspend();
  assert(!ledc_timer_active);
  speaker_tone_fault_once = true;
  assert(!StickSoundTestTone());
  assert(!StickSoundIsBusy() && !power.levels[2] && !power.levels[3]);
  // A new panel initialization reads the persisted Light setting.
  assert(StickDisplayPanelInit() && !StickDisplayIsDark());
  return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="stickwalker-power-audit-") as temp:
        folder = Path(temp)
        for name, contents in HEADERS.items():
            target = folder / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(contents)
        (folder / "trace.cpp").write_text(HARNESS)
        binary = folder / "trace"
        subprocess.run([
            "c++", "-std=c++17", "-DPW_STICK_S3",
            f"-I{folder}", f"-I{ROOT / 'include'}", f"-I{ROOT / 'stick'}",
            str(folder / "trace.cpp"),
            *[str(ROOT / "stick" / name) for name in (
                "board_runtime.cpp", "display_bus.cpp", "display_panel.cpp",
                "sound_bridge.cpp", "power_sleep.cpp", "peripheral_power.cpp",
                "backlight.cpp")],
            "-o", str(binary),
        ], check=True)
        rows = {}
        for case in ("quiet", "serial", "init-error", "persistent-error",
                     "sleep-error", "sleep-reject", "sleep-short", "queued-wake", "vbus-error", "vbus-transient", "lit", "moving", "supply-error", "codec-error", "supply-on-error", "speaker-error"):
            rows[case] = json.loads(subprocess.check_output([str(binary), case]))
        assert rows["quiet"]["sleeps"] > 0, "control must reach sleep"
        assert rows["vbus-error"]["sleeps"] == 0, "unknown USB power must veto sleep"
        assert rows["vbus-transient"]["sleeps"] > 0, "valid battery observation must clear unknown-power veto"
        assert not rows["supply-error"]["l3b_on"], "retry failed rail shutdown"
        assert not rows["codec-error"]["l3b_on"], "retry failed codec shutdown"
        assert rows["lit"]["sleeps"] > 0, "visible image must permit CPU sleep"
        assert rows["lit"]["l3b_on"], "visible LCD must retain its supply"
        assert rows["moving"]["main_runs"] >= 18, "keep motion sample cadence"
        assert rows["queued-wake"]["main_runs"] >= 18, "queued wake must not wait for stale one-second deadline"
        assert rows["init-error"]["sleeps"] > 0, "retry transient setup failure"
        assert rows["sleep-error"]["sleeps"] > 0, "recover failed sleep entry"
        for case in ("sleep-reject", "sleep-short"):
            assert rows[case]["sleeps"] >= 9, "ordinary rejection must not impose a one-second sleep veto"
            assert rows[case]["setup_attempts"] == 1, "ordinary rejection must not reset wake configuration"
        assert rows["persistent-error"]["setup_attempts"] <= 3, "bound retry rate"
        assert rows["quiet"]["lcd_sleep_calls"] > 0
        assert not rows["quiet"]["l3b_on"]
        assert rows["quiet"]["speaker_end_calls"] == 1
        assert rows["quiet"]["codec_writes_during_shutdown"] == 15
        findings = {
            "display_shutdown_incomplete": rows["quiet"]["brightness"] == 0
            and rows["quiet"]["lcd_sleep_calls"] == 0
            and rows["quiet"]["l3b_on"],
            "audio_shutdown_incomplete": not rows["quiet"]["amp_on_after_600ms"]
            and rows["quiet"]["speaker_end_calls"] == 0
            and rows["quiet"]["codec_writes_during_shutdown"] == 0,
            "unconsumed_serial_vetoes_sleep": rows["serial"]["sleeps"] == 0
            and rows["serial"]["serial_bytes"] == 1,
            "sleep_init_failure_is_permanent": rows["init-error"]["sleeps"] == 0
            and rows["init-error"]["main_runs"] > 0,
        }
        print(json.dumps({"trace": rows, "power_policy_violations": findings}, indent=2))
        raise SystemExit(1 if any(findings.values()) else 0)


if __name__ == "__main__":
    main()
