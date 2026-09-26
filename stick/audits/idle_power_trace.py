#!/usr/bin/env python3
"""Trace real production adapter paths on a host, without a device or save data.

This is a lifecycle probe, not a current or battery simulator. Hardware and
native foreground work are stubbed; the production runtime, display adapter,
sound adapter, and sleep scheduler are compiled unchanged. The speaker stub
records begin/stop/end calls. See the separately audited M5Unified implementation
for the electrical consequences of those calls.
"""

from pathlib import Path
import json
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
HEADERS = {
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
inline void setCpuFrequencyMhz(int) {}
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
namespace lgfx {
struct LGFX_Device {
  unsigned brightness = 68;
  unsigned sleep_calls = 0;
  void setBrightness(unsigned value) { brightness = value; }
  void setSleep(bool on) { sleep_calls += on; }
  void sleep() { ++sleep_calls; }
  void wakeup() {}
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
  template<class... T> void pushImage(T...) {}
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
struct I2CStub {
  unsigned writes = 0;
  uint8_t codec[256] = {};
  bool writeRegister8(unsigned address, unsigned reg, uint8_t value, unsigned) {
    if (address == 0x18) { ++writes; codec[reg] = value; }
    return true;
  }
};
extern I2CStub In_I2C;
}
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
  bool begin() { ++begin_calls; return true; }
  void end() { ++end_calls; }
  void stop() { ++stop_calls; }
  void setVolume(unsigned) {}
  template<class... T> bool tone(T...) { return true; }
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
enum gpio_num_t { GPIO_NUM_11 = 11, GPIO_NUM_12 = 12 };
inline int rtc_gpio_pullup_en(gpio_num_t) { return sleep_init_failed ? -1 : 0; }
inline int rtc_gpio_pulldown_dis(gpio_num_t) { return 0; }
""",
    "esp_sleep.h": r"""
#pragma once
#include <cstdint>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_PD_DOMAIN_RTC_PERIPH 0
#define ESP_PD_OPTION_ON 1
#define ESP_EXT1_WAKEUP_ANY_LOW 0
#define ESP_SLEEP_WAKEUP_TIMER 0
#define ESP_SLEEP_WAKEUP_EXT1 1
extern uint64_t clock_us, timer_duration_us;
extern unsigned completed_sleeps;
inline int esp_sleep_pd_config(int, int) { return 0; }
inline int esp_sleep_enable_ext1_wakeup_io(uint64_t, int) { return 0; }
inline int esp_sleep_enable_timer_wakeup(uint64_t duration) {
  timer_duration_us = duration; return 0;
}
inline int esp_light_sleep_start() {
  clock_us += timer_duration_us; ++completed_sleeps; return 0;
}
inline int esp_sleep_get_wakeup_cause() { return ESP_SLEEP_WAKEUP_TIMER; }
""",
}

HARNESS = r"""
#include <Arduino.h>
#include <M5Unified.h>
#include "display_bus.h"
#include "sound_bridge.h"
#include <cstdio>
#include <cstring>
uint64_t clock_us = 100000, timer_duration_us = 0;
int serial_bytes = 0;
bool sleep_init_failed = false;
unsigned completed_sleeps = 0;
unsigned main_runs = 0;
SerialStub Serial;
M5Stub M5;
namespace m5 { I2CStub In_I2C; }
lgfx::LGFX_Device screen;
m5::M5PM1_Class power;
bool StickBoardBegin() {
  // This stub isolates runtime behavior. board_hal.cpp independently shows
  // that setup enables GPIO2 and never disables it.
  power.levels[m5::M5PM1_Class::gpio2] = true;
  return true;
}
lgfx::LGFX_Device *StickBoardScreen() { return &screen; }
m5::M5PM1_Class &StickBoardPower() { return power; }
extern "C" {
void StickPortSetup();
void StickPortLoop();
void BeepAdvance() {}
void StickPortBoot() {
  StickSoundInit();
  IO.PDR1.BIT.B1 = 0;
  StickDisplayWrite(0xa9); // actual native display power-save command
}
int StickEepromMount() { return 1; }
void StickEepromDefer(int) {}
u16 StickBatteryMillivolts() { return 4000; }
int StickForegroundIsIr() { return 0; }
int StickForegroundIsMain() { return 1; }
int StickForegroundIsBeep() { return 0; }
int StickForegroundIsInactive() { return 1; }
unsigned StickForegroundUiFrame() { return 0; }
void StickForegroundQuarterSecond() {}
void StickForegroundSecond() {}
void StickForegroundRun() { ++main_runs; }
void StickForegroundDeviceMenuClosed() {}
void StickInputPoll(unsigned long) {}
int StickInputWakeScanActive() { return 0; }
int StickMenuRequested() { return 0; }
void StickInputMenuMode(int) {}
u8 StickInputTakeMenuButtons() { return 0; }
u8 StickInputProfile() { return 0; }
u8 StickInputOrientation() { return 0; }
u8 StickInputChordWindowIndex() { return 0; }
int StickInputConfigure(u8, u8, u8) { return 1; }
}
int main(int argc, char **argv) {
  const bool serial_case = argc > 1 && !strcmp(argv[1], "serial");
  const bool error_case = argc > 1 && !strcmp(argv[1], "init-error");
  sleep_init_failed = error_case;
  StickPortSetup();
  if (serial_case) serial_bytes = 1;
  const uint64_t end = clock_us + 1200000;
  unsigned guard = 0;
  while (clock_us < end && ++guard < 10000) StickPortLoop();
  if (guard == 10000) return 2;
  std::printf("{\"sleeps\":%u,\"main_runs\":%u,\"serial_bytes\":%d,\"brightness\":%u,"
              "\"lcd_sleep_calls\":%u,\"l3b_on\":%s,",
              completed_sleeps, main_runs, serial_bytes, screen.brightness,
              screen.sleep_calls, power.levels[2] ? "true" : "false");
  StickSoundEnable();
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
                "sound_bridge.cpp", "power_sleep.cpp")],
            "-o", str(binary),
        ], check=True)
        rows = {}
        for case in ("quiet", "serial", "init-error"):
            rows[case] = json.loads(subprocess.check_output([str(binary), case]))
        assert rows["quiet"]["sleeps"] > 0, "control must reach sleep"
        findings = {
            "display_shutdown_incomplete": rows["quiet"]["brightness"] == 0
            and rows["quiet"]["lcd_sleep_calls"] == 0
            and rows["quiet"]["l3b_on"],
            "audio_shutdown_incomplete": not rows["quiet"]["amp_on_after_600ms"]
            and rows["quiet"]["speaker_end_calls"] == 0
            and rows["quiet"]["codec_writes_during_shutdown"] == 0,
            "unconsumed_serial_vetoes_sleep": rows["serial"]["sleeps"] == 0
            and rows["serial"]["serial_bytes"] == 1,
            "sleep_init_failure_is_silent": rows["init-error"]["sleeps"] == 0
            and rows["init-error"]["main_runs"] > 0,
        }
        print(json.dumps({"trace": rows, "power_policy_violations": findings}, indent=2))
        raise SystemExit(1 if any(findings.values()) else 0)


if __name__ == "__main__":
    main()
