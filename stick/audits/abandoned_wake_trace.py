#!/usr/bin/env python3
"""Exercise abandoned and accepted gestures through the real input/panel adapters.

Only hardware and native application callbacks are stubbed. This checks panel
ownership, commands and timing, not current, physical switches or appearance.
"""
import json
from pathlib import Path
import subprocess
import tempfile

from idle_power_trace import HEADERS, ROOT

HARNESS = r'''
#include <Arduino.h>
#include <M5Unified.h>
#include "input_bridge.h"
#include "display_panel.h"
#include "display_bus.h"
#include "peripheral_power.h"
#include "backlight.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
uint64_t clock_us=100000;
bool main_down=false, side_down=false;
bool supply_on_fault=false;
int serial_bytes=0;
SerialStub Serial;
unsigned cpu_mhz=80;
bool speaker_begin_fault_once=false, speaker_tone_fault_once=false;
bool ledc_timer_active=false, ledc_update_fault_once=false;
M5Stub M5;
namespace m5 { I2CStub In_I2C; bool codec_fault_once=false; }
lgfx::LGFX_Device screen;
m5::M5PM1_Class power;
unsigned wakes=0;
lgfx::LGFX_Device *StickBoardScreen() { return &screen; }
m5::M5PM1_Class &StickBoardPower() { return power; }
bool StickBoardPeripheralSupply(bool on) {
  if (on && supply_on_fault) return false;
  power.levels[2]=on; return true;
}
void StickBoardDisplayReset(bool) {}
bool StickBoardDisplayInitRegisters() { return screen.init(); }
extern "C" void StickForegroundWakeDisplay() {
  ++wakes; StickDisplayWrite(0xe1); StickDisplayPanelSetBacklight(1);
}
extern "C" void StickForegroundCenterWake() {}
void poll(unsigned ms) {
  clock_us += ms*1000;
  StickInputPoll(millis());
  StickDisplayPowerService();
}
int main(int argc,char **argv) {
  assert(argc==3 || argc==4);
  const unsigned release_ms=std::atoi(argv[1]);
  const bool sound_owner=std::atoi(argv[2]);
  const bool fault=argc==4;
  StickPeripheralPowerInit();
  power.levels[2]=true;
  assert(StickDisplayPanelInit());
  StickInputInit();
  if (sound_owner) assert(StickPeripheralAcquire(StickPeripheral::Sound));
  StickDisplayWrite(0xa9); StickDisplayPanelSetBacklight(0);
  assert(!StickDisplayPanelIsReady());
  for (unsigned i=0;i<12;++i) poll(5); // input re-arm
  supply_on_fault=fault;
  main_down=true;
  for (unsigned i=0;i<release_ms;i+=5) poll(5);
  if (release_ms>=550) {
    assert(wakes==1 && StickDisplayPanelIsReady() && screen.brightness);
    main_down=false; poll(5);
    assert(StickDisplayPanelIsReady() && screen.brightness);
    std::printf("{\"hold_ms\":%u,\"sound_owner\":%s,\"accepted\":true}\n",
                release_ms,sound_owner?"true":"false");
    return 0;
  }
  assert(wakes==0 && screen.brightness==0);
  const unsigned on_at_release=screen.display_on_calls;
  main_down=false; poll(5);
  for (unsigned i=0;i<140;i+=5) poll(5);
  // Allow a pending Sleep-out to settle before Sleep-in; never send DISPON
  // after abandonment, and never wait out a gratuitous 750 ms tail.
  assert(!StickDisplayPanelIsReady());
  assert(power.levels[2]==sound_owner);
  assert(screen.display_on_calls==on_at_release);
  if (fault) {
    supply_on_fault=false;
    clock_us+=1100000; StickPeripheralPowerService();
    assert(!power.levels[2] && "abandonment must drop even a failed acquire claim");
  }
  const unsigned init_before=screen.init_calls;
  main_down=true;
  for (unsigned i=0;i<560;i+=5) poll(5);
  assert(wakes==1 && StickDisplayPanelIsReady() && screen.brightness);
  assert(screen.transaction_depth==0 && screen.unselected_commands==0);
  assert(StickDisplayPresent());
  assert(screen.init_calls>=init_before);
  main_down=false; poll(5);
  assert(StickDisplayPanelIsReady() && screen.brightness);
  std::printf("{\"hold_ms\":%u,\"sound_owner\":%s,\"cancelled\":true,\"next_hold_accepted\":true}\n",
              release_ms,sound_owner?"true":"false");
}
'''


def input_headers():
    headers = dict(HEADERS)
    headers['Arduino.h'] = headers['Arduino.h'].replace(
        'inline int digitalRead(int) { return 1; }',
        'extern bool main_down, side_down;\n'
        'inline int digitalRead(int pin) { return !(pin==11 ? main_down : side_down); }\n'
        '#define INPUT_PULLUP 1\ninline void pinMode(int,int) {}')
    headers['M5Unified.h'] = headers['M5Unified.h'].replace(
        'unsigned transaction_depth = 0, unselected_commands = 0;',
        'unsigned transaction_depth = 0, unselected_commands = 0, display_on_calls=0;')
    headers['M5Unified.h'] = headers['M5Unified.h'].replace(
        'if (command == 0x29) output_enabled = true;',
        'if (command == 0x29) { output_enabled = true; ++display_on_calls; }')
    headers['M5Unified.h'] = headers['M5Unified.h'].replace(
        'uint8_t codec[256] = {};', 'uint8_t codec[256] = {}, pmic[256] = {};').replace(
        '*value = address == 0x18 ? codec[reg] : 0; return true;',
        '*value = address == 0x18 ? codec[reg] : pmic[reg]; return true;').replace(
        '    if (address == 0x18) {',
        '    if (address == 0x6e) pmic[reg]=value;\n    if (address == 0x18) {')
    headers['M5Unified.h'] = headers['M5Unified.h'].replace(
        '  bool writeRegister8(unsigned address',
        '  bool writeRegister(unsigned address,unsigned reg,const uint8_t *value,unsigned,unsigned hz) {\n'
        '    return writeRegister8(address,reg,*value,hz);\n  }\n'
        '  bool writeRegister8(unsigned address')
    return headers


def main():
    with tempfile.TemporaryDirectory(prefix='stickwalker-abandoned-wake-') as temp:
        folder = Path(temp)
        headers = input_headers()
        for name, contents in headers.items():
            target = folder / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(contents)
        (folder / 'trace.cpp').write_text(HARNESS)
        binary = folder / 'trace'
        subprocess.run(['c++', '-std=c++17', '-DPW_STICK_S3', f'-I{folder}',
            f'-I{ROOT / "include"}', f'-I{ROOT / "stick"}',
            str(folder / 'trace.cpp'), *[str(ROOT / 'stick' / name) for name in
                ('input_bridge.cpp', 'controls.cpp', 'display_bus.cpp',
                 'display_panel.cpp', 'peripheral_power.cpp', 'backlight.cpp')],
            '-o', str(binary)], check=True)
        cases = []
        for sound_owner in (0, 1):
            for hold in (5, 25, 100, 180, 250, 450, 550):
                cases.append(json.loads(subprocess.check_output(
                    [str(binary), str(hold), str(sound_owner)], text=True)))
        case = json.loads(subprocess.check_output(
            [str(binary), '10', '0', 'supply-on-failure'], text=True))
        case['supply_on_failure'] = True
        cases.append(case)
        print(json.dumps({'passed': True, 'cases': cases}, indent=2))


if __name__ == '__main__':
    main()
