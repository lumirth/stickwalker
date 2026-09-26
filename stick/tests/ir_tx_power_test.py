#!/usr/bin/env python3
"""Compile real TX code against a driver boundary with lifecycle faults."""

import importlib.util
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "power_trace", ROOT / "stick/audits/idle_power_trace.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
headers = dict(module.HEADERS)
headers["M5Unified.h"] = headers["M5Unified.h"].replace(
    "bool setExtOutput(bool) { return true; }",
    "bool ext_on = false; bool setExtOutput(bool on) { ext_on = on; return true; }"
    " bool getExtOutput() { return ext_on; }")
headers["driver/gpio.h"] = r"""
#pragma once
#define ESP_OK 0
#define GPIO_NUM_46 46
#define GPIO_MODE_OUTPUT 1
inline int gpio_set_level(int, int) { return 0; }
inline int gpio_set_direction(int, int) { return 0; }
"""
headers["driver/rmt_encoder.h"] = r"""
#pragma once
#include <cstdint>
#include <cstddef>
typedef int esp_err_t;
typedef void *rmt_encoder_handle_t;
struct rmt_copy_encoder_config_t {};
extern bool encoder_fail;
extern unsigned encoder_deletes;
inline int rmt_new_copy_encoder(const rmt_copy_encoder_config_t *, void **p) {
  if (encoder_fail) { encoder_fail=false; return -1; }
  *p=reinterpret_cast<void *>(2); return 0;
}
inline int rmt_del_encoder(void *) { ++encoder_deletes; return 0; }
"""
headers["driver/rmt_tx.h"] = r"""
#pragma once
#include "rmt_encoder.h"
#include <cassert>
#include <vector>
#define RMT_CLK_SRC_DEFAULT 0
typedef void *rmt_channel_handle_t;
union rmt_symbol_word_t {
  uint32_t val;
  struct { unsigned duration0:15, level0:1, duration1:15, level1:1; };
};
struct rmt_tx_channel_config_t {
  int gpio_num, clk_src, resolution_hz, mem_block_symbols, trans_queue_depth;
};
struct rmt_transmit_config_t { struct { unsigned eot_level; } flags; };
extern bool channel_enabled, enable_fail;
extern unsigned channel_creates, channel_deletes, enables, disables;
extern std::vector<rmt_symbol_word_t> transmitted;
inline int rmt_new_tx_channel(const rmt_tx_channel_config_t *, void **p) {
  ++channel_creates; *p=reinterpret_cast<void *>(1); return 0;
}
inline int rmt_enable(void *) {
  if (enable_fail) { enable_fail=false; return -1; }
  assert(!channel_enabled); channel_enabled=true; ++enables; return 0;
}
inline int rmt_disable(void *) {
  assert(channel_enabled); channel_enabled=false; ++disables; return 0;
}
inline int rmt_del_channel(void *) { ++channel_deletes; return 0; }
inline int rmt_transmit(void *, void *, const void *p, size_t size,
                        const rmt_transmit_config_t *cfg) {
  assert(channel_enabled && cfg->flags.eot_level == 0);
  auto *s=static_cast<const rmt_symbol_word_t *>(p);
  transmitted.assign(s, s+size/sizeof(*s)); return 0;
}
inline int rmt_tx_wait_all_done(void *, int) { return 0; }
"""
headers["esp_heap_caps.h"] = r"""
#pragma once
#include <cstdlib>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
extern unsigned allocations;
inline void *heap_caps_malloc(size_t n, unsigned) { ++allocations; return malloc(n); }
"""
harness = r"""
#include <Arduino.h>
#include <M5Unified.h>
#include <driver/rmt_tx.h>
#include "ir_tx.h"
#include <cstring>
uint64_t clock_us=0;
unsigned cpu_mhz=240, allocations=0;
bool encoder_fail=false, enable_fail=false, channel_enabled=false;
unsigned channel_creates=0, channel_deletes=0, encoder_deletes=0;
unsigned enables=0, disables=0;
std::vector<rmt_symbol_word_t> transmitted;
m5::M5PM1_Class power;
m5::M5PM1_Class &StickBoardPower() { return power; }
int main(int argc, char **argv) {
  encoder_fail = argc>1 && !strcmp(argv[1], "encoder-fault");
  enable_fail = argc>1 && !strcmp(argv[1], "enable-fault");
  if (encoder_fail || enable_fail) {
    assert(!pw_stick::prepare_ir_tx());
    assert(channel_deletes==1 && !channel_enabled);
  }
  assert(pw_stick::prepare_ir_tx() && channel_enabled && power.ext_on);
  const unsigned created=channel_creates;
  uint8_t logical[136];
  for (unsigned i=0;i<sizeof(logical);++i) logical[i]=uint8_t(i*73+19);
  assert(pw_stick::transmit_logical(logical, sizeof(logical)));
  assert(transmitted.size()==1360);
  uint64_t ticks=0;
  for (unsigned i=0;i<transmitted.size();++i) {
    auto s=transmitted[i];
    unsigned bit=i%10;
    unsigned wire=logical[i/10]^0xaa;
    bool mark=bit==9 || (bit>0 && (wire & (1u<<(bit-1))));
    assert(s.level0==!mark && s.level1==0 && s.duration0==130);
    ticks+=s.duration0+s.duration1;
  }
  assert(ticks==uint64_t(1360)*80000000/115200);
  assert(pw_stick::suspend_ir_tx() && !channel_enabled);
  assert(pw_stick::suspend_ir_tx() && disables==1);
  assert(!pw_stick::transmit_logical(logical, 1));
  power.setExtOutput(false);  // session owner removes the 5 V rail
  assert(pw_stick::prepare_ir_tx() && channel_enabled && power.ext_on);
  assert(channel_creates==created && allocations==1);
  assert(pw_stick::transmit_logical(logical, 1));
  assert(pw_stick::suspend_ir_tx() && enables==2 && disables==2);
}
"""

with tempfile.TemporaryDirectory(prefix="stickwalker-tx-power-") as temp:
    folder = Path(temp)
    for name, contents in headers.items():
        path = folder / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents)
    (folder / "test.cpp").write_text(harness)
    binary = folder / "test"
    subprocess.run(["c++", "-std=c++17", f"-I{folder}",
                    f"-I{ROOT / 'stick'}", str(folder / "test.cpp"),
                    str(ROOT / "stick/ir_tx.cpp"), "-o", str(binary)], check=True)
    for case in ("normal", "encoder-fault", "enable-fault"):
        subprocess.run([str(binary), case], check=True)
        print(f"{case}: PASS")
