#!/usr/bin/env python3
"""Run native EEPROM scores through the real buzzer and Stick sound adapter.

Simulated time stalls model foreground/display work after enabling playback.
The output stub records actual tone/stop command lifetimes, not audibility or
I2S current. Compare each note with the same score serviced without a stall.
No device or save is modified. Scores are read from authoritative HGSS source.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from idle_power_trace import HEADERS, ROOT
import sys
sys.path.insert(0, str(ROOT / 'stick'))
from hgss_image import sound_bytes

NATIVE = r'''
#include "project.h"
#include "application/pw_buzzer.h"
#include "eeprom_map.h"
#include <string.h>
PresentationState g_ui;
Workspace g_work;
const Note *g_note;
static const u8 sounds[] = { SOUND_BYTES };
void EepromRead(u16 address, void *out, u16 length) {
  memcpy(out, sounds + address - EEPROM_SOUND_DIRECTORY, length);
}
void load_score(unsigned id) { BeepLoadScore(id); }
unsigned has_score(void) { return BeepHasScore(); }
void init_score(void) { BeepInit(); BeepSetOutputMode(2); }
'''

HARNESS = r'''
#include <M5Unified.h>
#include "sound_bridge.h"
#include "peripheral_power.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
uint64_t clock_us = 100000;
struct Tone { uint64_t start, end; float hz; };
std::vector<Tone> notes;
bool speaker_begin_fault_once=false, speaker_tone_fault_once=false;
M5Stub M5;
namespace m5 { I2CStub In_I2C; bool codec_fault_once=false; }
m5::M5PM1_Class power;
m5::M5PM1_Class &StickBoardPower() { return power; }
bool StickBoardPeripheralSupply(bool on) { power.levels[2]=on; return true; }
void record_tone(float hz, int channel, bool replace) {
  // M5's automatic channel selection takes a free channel even for a
  // preempting tone. Native Timer W has exactly one physical voice.
  if (channel != 0 || !replace) {
    std::fprintf(stderr,"native single voice required: channel=%d replace=%d\n",channel,replace);
    std::exit(3);
  }
  if (!notes.empty() && !notes.back().end) notes.back().end=clock_us;
  notes.push_back({clock_us,0,hz});
}
void record_stop() {
  if (!notes.empty() && !notes.back().end) notes.back().end=clock_us;
}
extern "C" void load_score(unsigned);
extern "C" unsigned has_score();
extern "C" void init_score();
int main(int argc,char **argv) {
  const unsigned id=std::atoi(argv[1]);
  const uint64_t stall=std::strtoul(argv[2],nullptr,10);
  const bool cold=std::atoi(argv[3]);
  const bool during=std::atoi(argv[4]);
  StickPeripheralPowerInit(); init_score();
  if (!cold) {
    // Reuse a real preceding cue's compare value and amplifier hold.
    load_score(2); StickSoundEnable();
    unsigned prime=0;
    while (has_score() && ++prime<100000) { StickSoundService(); clock_us+=100; }
    if (prime==100000) return 2;
    StickSoundDisable(); clock_us+=100000; notes.clear();
  }
  load_score(id); StickSoundEnable();
  clock_us+=stall;
  unsigned steps=0;
  bool stalled=false;
  while (has_score() && ++steps < 100000) {
    StickSoundService();
    // An existing note continues sounding during a blocked foreground.
    if (during && !stalled && !notes.empty()) {
      clock_us+=80000; stalled=true;
    }
    clock_us+=100;
  }
  if (steps==100000) return 2;
  StickSoundDisable();
  std::printf("{\"score\":%u,\"stall_us\":%llu,\"cold\":%s,\"during\":%s,\"notes\":[",
    id,(unsigned long long)stall,cold?"true":"false",during?"true":"false");
  for (unsigned i=0;i<notes.size();++i) {
    if (i) std::printf(",");
    std::printf("{\"hz\":%.3f,\"start_us\":%llu,\"duration_us\":%llu}",
      notes[i].hz,(unsigned long long)notes[i].start,
      (unsigned long long)(notes[i].end-notes[i].start));
  }
  std::puts("]}");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hgss', type=Path,
                        default=Path('/Users/lu/Downloads/PokemonHGSS_Source_Code/pokemon_gs 2'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = {'passed': False, 'cases': []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='stickwalker-sound-') as temp:
        folder = Path(temp)
        headers = dict(HEADERS)
        h = headers['M5Unified.h']
        h = h.replace('struct SpeakerStub {',
                      'extern uint64_t clock_us;\nvoid record_tone(float,int,bool);\nvoid record_stop();\nstruct SpeakerStub {')
        h = h.replace('void stop() { ++stop_calls; }',
                      'void stop() { ++stop_calls; record_stop(); }')
        h = h.replace('template<class... T> bool tone(T...) {',
                      'template<class... T> bool tone(float hz, uint32_t duration, int channel, bool replace, T...) { record_tone(hz,channel,replace);')
        h = h.replace('  void setVolume(unsigned) {}',
                      '  void setVolume(unsigned) {}\n  bool tone(float, uint32_t) { return true; }')
        headers['M5Unified.h'] = h
        for name, contents in headers.items():
            target = folder / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(contents)
        archive = sound_bytes(args.hgss)
        (folder / 'native.c').write_text(NATIVE.replace(
            'SOUND_BYTES', ','.join(str(x) for x in archive)))
        (folder / 'trace.cpp').write_text(HARNESS)
        flags = ['-DPW_STICK_S3', f'-I{folder}', f'-I{ROOT / "include"}',
                 f'-I{ROOT / "stick"}', f'-I{ROOT}', f'-I{ROOT / "build/6.02.02"}']
        objects = []
        for i, source in enumerate([folder / 'native.c',
                ROOT / 'src/application/pw_buzzer.c',
                ROOT / 'src/application/pw_builtin.c']):
            obj = folder / f'{i}.o'
            subprocess.run(['cc', '-std=c99', '-Wno-pointer-to-int-cast',
                            *flags, '-c', str(source), '-o', str(obj)], check=True)
            objects.append(str(obj))
        binary = folder / 'trace'
        subprocess.run(['c++', '-std=c++17', *flags, str(folder / 'trace.cpp'),
                        str(ROOT / 'stick/sound_bridge.cpp'),
                        str(ROOT / 'stick/peripheral_power.cpp'), *objects,
                        '-o', str(binary)], check=True)
        try:
            for score in range(16):
                baseline = json.loads(subprocess.check_output(
                    [str(binary), str(score), '0', '0', '0']))
                for cold in (False, True):
                    for stall in (0, 8000, 20000, 80000, 200000):
                        row = json.loads(subprocess.check_output([
                            str(binary), str(score), str(stall), str(int(cold)), '0']))
                        expected = baseline['notes']
                        row['passed'] = len(row['notes']) == len(expected) and all(
                            a['hz'] == b['hz'] and
                            abs(a['duration_us'] - b['duration_us']) <= 200
                            for a, b in zip(row['notes'], expected))
                        row['passed'] = row['passed'] and all(
                            abs((row['notes'][i]['start_us'] - row['notes'][i-1]['start_us'] -
                                 row['notes'][i-1]['duration_us']) -
                                (expected[i]['start_us'] - expected[i-1]['start_us'] -
                                 expected[i-1]['duration_us'])) <= 200
                            for i in range(1, len(expected)))
                        report['cases'].append(row)
                row = json.loads(subprocess.check_output([
                    str(binary), str(score), '0', '0', '1']))
                row['passed'] = len(row['notes']) == len(baseline['notes']) and all(
                    a['hz'] == b['hz'] and
                    (a['duration_us'] >= b['duration_us'] - 200 if i == 0 else
                     abs(a['duration_us'] - b['duration_us']) <= 200)
                    for i, (a, b) in enumerate(zip(row['notes'], baseline['notes'])))
                report['cases'].append(row)
            failures = [r for r in report['cases'] if not r['passed']]
            assert not failures, f'{len(failures)} score timing failures; first: {failures[0] if failures else None}'
            report['passed'] = True
        except Exception as exc:
            report['error'] = str(exc)
            raise
        finally:
            args.output.write_text(json.dumps(report, indent=2) + '\n')
            print(json.dumps({'passed': report['passed'], 'cases': len(report['cases']),
                              'last': report['cases'][-1] if report['cases'] else None}))


if __name__ == '__main__':
    main()
