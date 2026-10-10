#!/usr/bin/env python3
"""Exercise receive-worker stop and reconnect against task API substitutes."""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

from ir_transport_test_support import ROOT as root
from ir_transport_test_support import SOURCE as source
from ir_transport_test_support import transport_function as function

cpp = r"""
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include "ir_rx_core.h"
#include "ir_gate.h"
#define GPIO_NUM_5 5
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define ESP_OK 0
#define pdPASS 1
using TaskHandle_t = void *;
struct gpio_config_t {
  uint64_t pin_bit_mask;
  int mode, pull_up_en, pull_down_en, intr_type;
};
uint64_t now_us = 0;
unsigned cpu_mhz = 240, creates = 0, deletes = 0, gpio_resets = 0;
unsigned park_delays = 0;
enum eTaskState { eRunning, eSuspended };
eTaskState task_state = eRunning;
bool finish_on_delay = true, rail = true;
uint8_t *ring = nullptr, *packet_bins = nullptr;
constexpr unsigned kRingSize = 32768;
volatile uint32_t produced = 0, event_write = 0, event_read = 0;
volatile bool failed = false, stop_requested = false, running = false,
              worker_ready = false, worker_done = false;
unsigned raw_events = 0, valid_bursts = 0, invalid_bursts = 0,
         self_echo_bursts = 0, overlong_bursts = 0, short_bursts = 0;
unsigned final_samples = 0, final_input_mask = 0, diagnostic_count = 0,
         burst_diagnostic_count = 0;
pw_stick::GateHealth final_health;
TaskHandle_t worker_handle = nullptr, protocol_handle = nullptr;
int GPIO;
struct Power {
  bool setExtOutput(bool on)
  {
    rail = on;
    return true;
  }
} power;
Power &StickBoardPower()
{
  return power;
}
namespace pw_stick {
void reset_rx_timing()
{
}
bool prepare_ir_tx()
{
  rail = true;
  return true;
}
bool suspend_ir_tx()
{
  return true;
}
} // namespace pw_stick
void StickSoundQuiesceForIr()
{
}
bool setCpuFrequencyMhz(unsigned mhz)
{
  cpu_mhz = mhz;
  return true;
}
unsigned getCpuFrequencyMhz()
{
  return cpu_mhz;
}
void *heap_caps_malloc(size_t n, unsigned)
{
  return malloc(n);
}
void gpio_reset_pin(int)
{
  ++gpio_resets;
}
void gpio_set_direction(int, int)
{
}
void gpio_pullup_dis(int)
{
}
void gpio_pulldown_dis(int)
{
}
void gpio_ll_set_level(int *, unsigned, int)
{
}
int gpio_config(const gpio_config_t *)
{
  return ESP_OK;
}
void sampler(void *)
{
}
TaskHandle_t xTaskGetCurrentTaskHandle()
{
  return reinterpret_cast<void *>(1);
}
int xTaskCreatePinnedToCore(void (*)(void *), const char *, unsigned, void *,
                            unsigned, TaskHandle_t *h, unsigned)
{
  *h = reinterpret_cast<void *>(uintptr_t(++creates + 1));
  worker_ready = true;
  task_state = eRunning;
  return pdPASS;
}
eTaskState eTaskGetState(TaskHandle_t)
{
  return task_state;
}
void vTaskDelete(TaskHandle_t)
{
  if (task_state != eSuspended) {
    std::fprintf(stderr, "FAIL: deleted a running worker; FreeRTOS delegates "
                         "its cleanup to the idle task\n");
    std::exit(1);
  }
  ++deletes;
}
int64_t esp_timer_get_time()
{
  return now_us;
}
void delayMicroseconds(unsigned n)
{
  now_us += n;
}
void delay(unsigned n)
{
  now_us += n * 1000;
  if (finish_on_delay) {
    worker_done = true;
    if (park_delays)
      --park_delays;
    else
      task_state = eSuspended;
  }
}
void core1_tick(bool)
{
}
"""
if "bool worker_parked(" in source:
    cpp += "\n" + function("bool worker_parked(")
cpp += (
    "\n"
    + function('extern "C" void StickIrConfigure(')
    + "\n"
    + function('extern "C" void StickIrStart(')
    + "\n"
    + function('extern "C" void StickIrStop(')
)
cpp += r"""
int main()
{
  StickIrConfigure();
  StickIrStart();
  assert(creates == 1 && worker_handle && running);
  finish_on_delay = false;
  const auto original = worker_handle;
  StickIrStop();
  if (!worker_handle || worker_handle != original) {
    std::fprintf(stderr,
                 "FAIL: stop timeout lost the live receiver task handle\n");
    return 1;
  }
  if (!stop_requested || cpu_mhz != 240 || !rail || !running || !failed) {
    std::fprintf(
        stderr,
        "FAIL: stop timeout changed resources still used by the receiver\n");
    return 1;
  }
  StickIrConfigure();
  StickIrStart();
  if (creates != 1 || !stop_requested || worker_handle != original) {
    std::fprintf(stderr, "FAIL: reconnect started a second receiver\n");
    return 1;
  }
  finish_on_delay = true;
  worker_done = true;
  task_state = eRunning;
  park_delays = 2;
  StickIrStop();
  assert(!worker_handle && !running && deletes == 1 && !rail && cpu_mhz == 80);
  StickIrConfigure();
  StickIrStart();
  assert(creates == 2 && worker_handle && running && !failed);
  StickIrStop();
  assert(deletes == 2);
  free(ring);
  free(packet_bins);
  std::puts("Worker timeout, reconnect exclusion and eventual cleanup: PASS");
}
"""

with tempfile.TemporaryDirectory(prefix="stickwalker-rx-lifecycle-") as temp:
    folder = Path(temp)
    (folder / "lifecycle.cpp").write_text(cpp)
    subprocess.run(
        [
            os.environ.get("CXX", "c++"),
            "-std=c++17",
            "-O1",
            "-g",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-I" + str(root / "stick"),
            str(folder / "lifecycle.cpp"),
            "-o",
            str(folder / "lifecycle"),
        ],
        check=True,
    )
    sys.exit(subprocess.run([str(folder / "lifecycle")]).returncode)
