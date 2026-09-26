#!/usr/bin/env python3
"""Probe a flashed bench build; never erase or import EEPROM.

Use the bench venv Python, --port, --output (prefer ignored stick/.build),
and one mode. USB-connected modes never permit sleep. battery-arm only
arms a finite disconnected trial; it does not report a passed trial.
battery-read retrieves a completed RTC record after reconnection/reset.
This reports lifecycle/clock/sampling evidence, not supply current.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import serial


def fields(line):
    return {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)(?:\s|$)', line)}


class Link:
    def __init__(self, port):
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=.05,
                                    write_timeout=1)
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.port = port
        self.serial.open()
        self.rows = []
        self.pending = bytearray()

    def collect(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                self.pending.extend(self.serial.read(self.serial.in_waiting or 1))
            except (OSError, serial.SerialException):
                time.sleep(.05)
                continue
            while b'\n' in self.pending:
                row, _, self.pending = self.pending.partition(b'\n')
                text = row.decode(errors='replace').strip()
                if text:
                    self.rows.append({'host_monotonic': time.monotonic(),
                                      'text': text})

    def command(self, text, seconds=1):
        self.serial.write(text.encode())
        self.collect(seconds)

    def close(self):
        self.serial.close()


def last(rows, prefix):
    found = [r['text'] for r in rows if r['text'].startswith(prefix)]
    assert found, 'Missing ' + prefix
    return found[-1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode', choices=['battery-arm', 'battery-read', 'audio', 'rx', 'boot'])
    p.add_argument('--port', required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    reset = [sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port',
             args.port, '--after', 'hard_reset', 'run']
    report = {'mode': args.mode, 'passed': False, 'before': [], 'recovered': []}
    link = None
    try:
        with args.output.with_suffix('.reset.log').open('w') as log:
            subprocess.run(reset, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=30)
        link = Link(args.port)
        link.collect(3)
        link.command('tbPso')
        assert not any('FATAL' in r['text'] for r in link.rows), 'Startup failed'
        assert 'cpu_mhz=80 ' in last(link.rows, 'PW_STICK_POWER_HW ')
        if args.mode == 'audio':
            link.command('zn', 2)
            link.command('tbPs')
            power = last(link.rows, 'PW_STICK_POWER_HW ')
            assert 'panel_awake=0 ' in power and 'gpio_out=00 ' in power, power
        if args.mode == 'battery-arm':
            link.command('Y')
            last(link.rows, 'PW_STICK_BATTERY_TRIAL_ARMED ')
            report['armed'] = True
            report['requires_disconnected_trial'] = True
        elif args.mode == 'battery-read':
            sleep = fields(last(link.rows, 'PW_STICK_SLEEP_PREVIOUS '))
            trial = fields(last(link.rows, 'PW_STICK_POWER_TRIAL_PREVIOUS '))
            report['sleep'] = sleep
            report['trial'] = trial
            assert sleep['phase'] == 2 and sleep['count'] > 10, sleep
            assert sleep['failures'] == 0, sleep
            assert sleep['error'] in (0, 258, 259), sleep
            assert trial['completed'] == 1 and trial['failures'] == 0, trial
            elapsed = trial['end_us'] - trial['start_us']
            samples = trial['end_reads'] - trial['start_reads']
            seconds = trial['end_seconds'] - trial['start_seconds']
            assert 5000000 <= elapsed <= 5200000, trial
            assert 4 <= seconds <= 6 and samples >= 4, trial
            report['samples'] = samples
            report['native_seconds'] = seconds
        elif args.mode == 'audio':
            for _ in range(2):
                link.command('a')
                link.command('sP')
                active = last(link.rows, 'PW_STICK_POWER_HW ')
                assert 'sound_busy=1 ' in active and 'gpio_out=0c ' in active, active
                link.collect(8)
                link.command('sP')
                idle = last(link.rows, 'PW_STICK_POWER_HW ')
                assert 'sound_busy=0 ' in idle and 'gpio_out=00 ' in idle, idle
                sound = last(link.rows, 'PW_STICK_SOUND ')
                assert 'ready=0 ' in sound and 'codec=0 ' in sound, sound
                assert 'begin_errors=0 ' in sound and 'power_errors=0 ' in sound, sound
            assert fields(sound)['begins'] >= 2, sound
        elif args.mode == 'rx':
            link.command('r', 3)
            link.command('tbPs')
            last(link.rows, 'PW_STICK_TRIAL_START')
            last(link.rows, 'PW_STICK_TRIAL_DONE')
            power = last(link.rows, 'PW_STICK_POWER_HW ')
            assert 'cpu_mhz=80 ' in power, power
            assert fields(last(link.rows, 'PW_STICK_POWER '))['ext_5v'] == 0
        if not report['before']:
            report['before'] = link.rows
        report['passed'] = args.mode != 'battery-arm'
    except Exception as exc:
        report['error'] = str(exc)
        if link:
            report['last_rows'] = link.rows
        raise
    finally:
        if link:
            link.close()
        args.output.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({k: v for k, v in report.items()
                          if k not in ('before', 'recovered', 'last_rows')}))


if __name__ == '__main__':
    main()
