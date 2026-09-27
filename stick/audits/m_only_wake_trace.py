#!/usr/bin/env python3
"""Real input adapter regression: only M wakes a dark game in every layout.

Hardware switch/PMIC reads are stubbed. Not a physical-switch or energy test.
"""
import json
from pathlib import Path
import subprocess
import tempfile
from abandoned_wake_trace import HARNESS, input_headers
from idle_power_trace import ROOT

POLICY = r'''
int main(int argc,char **argv) {
  const unsigned layout=std::atoi(argv[1]);
  const unsigned orientation=std::atoi(argv[2]);
  StickPeripheralPowerInit(); power.levels[2]=true;
  assert(StickDisplayPanelInit()); StickInputInit();
  assert(StickInputConfigure(layout, orientation, 0));
  StickDisplayWrite(0xa9); StickDisplayPanelSetBacklight(0);
  for (unsigned i=0;i<12;++i) poll(5);
  const unsigned reads=m5::In_I2C.button_reads;
  side_down=true; m5::In_I2C.pmic[0x48]=0x81;
  for (unsigned i=0;i<300;++i) {
    poll(5);
    assert(!StickInputWakeScanActive());
    assert(!StickInputLevels() && !StickMenuRequested());
  }
  assert(wakes==0 && !StickDisplayPanelIsReady() && !power.levels[2]);
  assert(m5::In_I2C.button_reads==reads && "no dark-screen PMIC polling");
  // Neither a held R nor L may prevent an intentional M wake.
  main_down=true;
  for (unsigned i=0;i<112;++i) poll(5);
  assert(wakes==1 && StickDisplayPanelIsReady());
  assert(!StickMenuRequested() && !StickInputLevels());
  main_down=false; side_down=false;
  m5::In_I2C.button_read_failures=4;
  // Reading the old L latch must discard it; a still-held L must not navigate.
  for (unsigned i=0;i<20;++i) {
    poll(5); assert(!StickMenuRequested() && !StickInputLevels());
  }
  m5::In_I2C.pmic[0x48]=0;
  for (unsigned i=0;i<20;++i) poll(5);
  assert(!StickMenuRequested() && !StickInputLevels());
  // R navigation and L/Settings controls remain available after wake.
  side_down=true; for(unsigned i=0;i<25;++i) poll(5);
  assert(StickInputLevels() == ((layout==0 || layout==2)?8:4));
  side_down=false; for(unsigned i=0;i<25;++i) poll(5);
  while(StickInputLevels()) {}
  StickInputMenuMode(1);
  StickDisplayWrite(0xa9); // Settings stays interactive despite native blank.
  m5::In_I2C.pmic[0x48]=0x80; poll(5);
  assert(StickInputTakeMenuButtons()==4);
  main_down=true; poll(5); poll(5);
  assert(StickInputTakeMenuButtons()==1);
  main_down=false; poll(5); poll(5);
  side_down=true; poll(5); poll(5);
  assert(StickInputTakeMenuButtons()==2);
  std::printf("{\"layout\":%u,\"orientation\":%u,\"passed\":true}\n",layout,orientation);
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='stickwalker-m-wake-') as temp:
        folder=Path(temp)
        headers=input_headers()
        headers['M5Unified.h']=headers['M5Unified.h'].replace(
            'uint8_t codec[256] = {}, pmic[256] = {};',
            'uint8_t codec[256] = {}, pmic[256] = {}; unsigned button_reads=0, button_read_failures=0;').replace(
            '*value = address == 0x18 ? codec[reg] : pmic[reg]; return true;',
            'if(address==0x6e && reg==0x48 && button_read_failures) { --button_read_failures; return false; } '
            '*value = address == 0x18 ? codec[reg] : pmic[reg]; '
            'if(address==0x6e && reg==0x48) { ++button_reads; pmic[reg]&=~0x80u; } return true;')
        for name,contents in headers.items():
            target=folder/name; target.parent.mkdir(parents=True,exist_ok=True)
            target.write_text(contents)
        (folder/'trace.cpp').write_text(HARNESS[:HARNESS.index('int main(')]+POLICY)
        binary=folder/'trace'
        subprocess.run(['c++','-std=c++17','-DPW_STICK_S3',f'-I{folder}',
            f'-I{ROOT / "include"}',f'-I{ROOT / "stick"}',str(folder/'trace.cpp'),
            *[str(ROOT/'stick'/name) for name in ('input_bridge.cpp','controls.cpp',
            'display_bus.cpp','display_panel.cpp','peripheral_power.cpp','backlight.cpp')],
            '-o',str(binary)],check=True)
        cases=[json.loads(subprocess.check_output([str(binary),str(layout),str(orientation)],text=True))
               for layout in range(4) for orientation in range(2)]
        print(json.dumps({'passed':True,'cases':cases},indent=2))

if __name__=='__main__':
    main()
