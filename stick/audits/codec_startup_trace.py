#!/usr/bin/env python3
"""Check the actual sound adapter's codec activation barrier and write recovery.

This is a register-order test, not an acoustic measurement. Emulate the real
0x0c POR value and an acknowledged first-write loss. The companion board
capture checks PCM feedback and microphone energy at the native cue pitch.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from idle_power_trace import HEADERS, ROOT

HARNESS = r'''
#include <M5Unified.h>
#include "sound_bridge.h"
#include "peripheral_power.h"
#include <cstdio>
#include <cstdlib>
uint64_t clock_us=100000;
bool speaker_begin_fault_once=false, speaker_tone_fault_once=false;
M5Stub M5;
namespace m5 {
bool codec_fault_once=false;
bool drop_stage_once=false, block_stage=false;
unsigned unsafe_starts=0, stage_writes=0;
I2CStub In_I2C;
}
m5::M5PM1_Class power;
m5::M5PM1_Class &StickBoardPower() { return power; }
bool StickBoardPeripheralSupply(bool on) { power.levels[2]=on; return true; }
extern "C" void BeepAdvance() {}
int main(int argc,char **argv) {
  const int fault=std::atoi(argv[1]);
  StickPeripheralPowerInit();
  m5::In_I2C.codec[0x0c]=0x20;
  StickSoundInit();
  m5::drop_stage_once=fault==1; m5::block_stage=fault==2;
  StickSoundEnable();
  bool pass=m5::unsafe_starts==0;
  if (fault==2) pass=pass && !power.levels[3] && M5.Speaker.end_calls>0;
  else {
    pass=pass && power.levels[3] && m5::In_I2C.codec[0x0c]==0;
    // Suspend/resume while the panel retains the rail.
    StickSoundDisable(); clock_us+=600000; StickSoundService();
    m5::In_I2C.codec[0x0c]=0x20;
    StickSoundEnable();
    pass=pass && m5::unsafe_starts==0 && m5::In_I2C.codec[0x0c]==0;
    // A true rail loss restores codec POR defaults.
    StickSoundQuiesceForIr(); StickPeripheralRelease(StickPeripheral::Display);
    m5::In_I2C.codec[0x0c]=0x20;
    StickSoundEnable();
    pass=pass && m5::unsafe_starts==0 && m5::In_I2C.codec[0x0c]==0;
  }
  std::printf("{\"fault\":%d,\"passed\":%s,\"unsafe_starts\":%u,\"stage_writes\":%u}\n",
    fault,pass?"true":"false",m5::unsafe_starts,m5::stage_writes);
  return pass?0:1;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge', type=Path, default=ROOT / 'stick/sound_bridge.cpp')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = {'passed': False, 'cases': []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='stickwalker-codec-') as temp:
        folder = Path(temp)
        headers = dict(HEADERS)
        header = headers['M5Unified.h']
        header = header.replace('extern bool codec_fault_once;',
            'extern bool codec_fault_once, drop_stage_once, block_stage;\n'
            'extern unsigned unsafe_starts, stage_writes;')
        header = header.replace('      ++writes;', '''      ++writes;
      if (reg==0x0c) {
        ++stage_writes;
        if (block_stage) return true;
        if (drop_stage_once) { drop_stage_once=false; return true; }
      }
      if (reg==0 && (value & 0x80) && codec[0x0c]!=0) ++unsafe_starts;''')
        headers['M5Unified.h'] = header
        for name, contents in headers.items():
            target = folder / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(contents)
        (folder / 'trace.cpp').write_text(HARNESS)
        binary = folder / 'trace'
        flags = ['-DPW_STICK_S3', f'-I{folder}', f'-I{ROOT / "include"}',
                 f'-I{ROOT / "stick"}', f'-I{ROOT}']
        subprocess.run(['c++', '-std=c++17', *flags, str(folder / 'trace.cpp'),
                        str(args.bridge), str(ROOT / 'stick/peripheral_power.cpp'),
                        '-o', str(binary)], check=True)
        for fault in range(3):
            result = subprocess.run([str(binary), str(fault)], capture_output=True, text=True)
            report['cases'].append(json.loads(result.stdout))
    report['passed'] = all(row['passed'] for row in report['cases'])
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))
    raise SystemExit(0 if report['passed'] else 1)


if __name__ == '__main__':
    main()
