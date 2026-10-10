#!/usr/bin/env python3
"""Run the production receive loop against timed input and a watchdog model."""

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
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include "ir_rx_core.h"
#define IRAM_ATTR
using TaskHandle_t = void *;
constexpr uint32_t kRingSize = 32768, kRingMask = kRingSize - 1;
constexpr unsigned kEventSlots = 8, kGateHz = 345600, kLowCutoff = 124,
                   kPacketGapGates = 120;
struct OpticalEvent {
  uint32_t first, last, end_gate;
  int64_t end_us;
};
OpticalEvent events[kEventSlots];
volatile uint32_t produced = 0, event_write = 0, event_read = 0;
volatile bool stop_requested = false, tx_busy = false, failed = false;
uint32_t self_echo_bursts = 0, short_bursts = 0, raw_events = 0,
         overlong_bursts = 0;
uint64_t cycles = 0, disabled_at = 0;
bool core1_enabled = true, interrupts_enabled = false;
unsigned calls = 0;
std::string mode;
constexpr uint64_t watchdog_cycles = uint64_t(300) * 240000;
int GPIO, SYSTIMER;
constexpr int SYSTIMER_ALARM_OS_TICK_CORE1 = 1;
void gpio_ll_output_enable(int *, unsigned)
{
}
void gpio_ll_output_disable(int *, unsigned)
{
}
uint32_t esp_cpu_get_cycle_count()
{
  return uint32_t(++cycles);
}
int64_t esp_timer_get_time()
{
  return cycles / 240;
}
void systimer_ll_enable_alarm_int(int *, int, bool enabled)
{
  if (core1_enabled && !enabled)
    disabled_at = cycles;
  core1_enabled = enabled;
}
void portENABLE_INTERRUPTS()
{
  interrupts_enabled = true;
}
void portDISABLE_INTERRUPTS()
{
  interrupts_enabled = false;
}
void xTaskNotifyGive(TaskHandle_t)
{
}
void vTaskDelay(unsigned)
{
  cycles += 240000;
}
namespace pw_stick {
struct GateHealth {
  uint32_t phase_late = 0, already_low = 0, timed_out = 0;
};
uint8_t measure_ir_gate(uint32_t, uint32_t next, uint32_t, GateHealth &,
                        uint8_t *destination)
{
  cycles += uint32_t(next - uint32_t(cycles));
  if (!core1_enabled && cycles - disabled_at >= watchdog_cycles)
    throw std::runtime_error("CPU1 interrupt watchdog reset");
  unsigned i = calls++;
  uint8_t value = 255;
  if (mode == "continuous-light")
    value = 0;
  else if (mode == "noisy-light")
    value = (i % 100 == 0) ? 0 : 255;
  else if ((mode == "max-packet" || mode == "dense-packet") && i < 136 * 30)
    value = mode == "max-packet" ? (i % 30 == 0 ? 0 : 255)
                                 : (i % 3 == 0 && (i / 3) % 10 != 9 ? 0 : 255);
  else if (mode == "cancelled-packet") {
    value = 0;
    if (i == 1023)
      stop_requested = true;
  }
  *destination = value;
  return value;
}
} // namespace pw_stick
"""
cpp += (
    "\n"
    + function("void core1_tick(")
    + "\n"
    + function("uint32_t IRAM_ATTR __attribute__((noinline)) aligned_next(")
    + "\n"
    + function("uint32_t IRAM_ATTR __attribute__((noinline)) run_gate_segment(")
)
cpp += r"""
int main(int argc, char **argv)
{
  mode = argc > 1 ? argv[1] : "idle";
  if (argc > 2)
    cycles = 0xfffff000ULL;
  const uint64_t started = cycles;
  static uint8_t bins[kRingSize];
  pw_stick::GateHealth health;
  try {
    run_gate_segment(bins, 0, kGateHz * 30, health, 1, true, true, true, 250,
                     reinterpret_cast<void *>(1));
    if (!core1_enabled)
      throw std::runtime_error("tick still masked");
    if (mode == "continuous-light" || mode == "noisy-light") {
      if (!failed || overlong_bursts != 1 || event_write != 0)
        throw std::runtime_error("oversized input was not rejected");
      if (calls > pw_stick::kMaxBurstGates + 1)
        throw std::runtime_error("oversized input exceeded capture capacity");
    }
    if ((mode == "max-packet" || mode == "dense-packet") &&
        (failed || event_write != 1))
      throw std::runtime_error("valid full-length packet was rejected");
    std::printf("%s: no reset; sampled %u gates (%llu us)\n", mode.c_str(),
                calls, (unsigned long long)((cycles - started) / 240));
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s: %s after %llu us\n", mode.c_str(), e.what(),
                 (unsigned long long)((cycles - started) / 240));
    return 1;
  }
}
"""

with tempfile.TemporaryDirectory(prefix="stickwalker-rx-watchdog-") as temp:
    folder = Path(temp)
    (folder / "watchdog.cpp").write_text(cpp)
    compiler = os.environ.get("CXX", "c++")
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-O2",
            "-g",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-I" + str(root / "stick"),
            str(folder / "watchdog.cpp"),
            "-o",
            str(folder / "watchdog"),
        ],
        check=True,
    )
    failures = []
    for case in (
        "idle",
        "max-packet",
        "dense-packet",
        "continuous-light",
        "noisy-light",
        "cancelled-packet",
    ):
        for wrap in (False, True):
            command = [str(folder / "watchdog"), case] + (["wrap"] if wrap else [])
            if subprocess.run(command).returncode:
                failures.append((case, wrap))
    if failures:
        print("Failed receive cases:", failures)
        sys.exit(1)
    print("Receive watchdog bounds: 12 cases passed.")
