#!/usr/bin/env python3
"""Exercise cold display restoration through bench-generated input holds.

Runs awake on USB; never enables processor sleep or imports/erases a save.
The M hold enters the physical-input polling path. These are software inputs,
not verification of actual switches or panel appearance. Dark L must be ignored; Settings is exercised after M wakes.
Retain failure output and restore production afterward.
"""
import argparse
import json
from pathlib import Path
import time
import re

from hardware_power_probe import Link, last


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--lcd-readback', action='store_true',
                        help='Require the ST7789P3 output-on and sleep-out bits')
    parser.add_argument('--abandoned-taps', type=int, default=0,
                        help='Check software-generated 40 ms M taps before valid holds')
    args = parser.parse_args()
    assert 1 <= args.repeats <= 20
    assert 0 <= args.abandoned_taps <= 20
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'cases': [], 'software_inputs': True}
    link = Link(args.port)
    try:
        link.collect(2)
        for repeat in range(args.abandoned_taps):
            link.command('zn', 1)
            link.command('P', .1)
            before = last(link.rows, 'PW_STICK_POWER_HW ')
            assert 'panel_awake=0 ' in before and 'gpio_out=00 ' in before, before
            link.command('V', .2)
            link.command('P', .1)
            after = last(link.rows, 'PW_STICK_POWER_HW ')
            case = {'repeat': repeat, 'input': 'abandoned M tap',
                    'before': before, 'after': after}
            case['passed'] = 'panel_awake=0 ' in after and 'gpio_out=00 ' in after
            report['cases'].append(case)
            assert case['passed'], case
        for repeat in range(args.repeats):
            for name, trigger in (('M hold', 'v'), ('settings after M wake', 'l')):
                link.command('zn', 1)
                link.command('P', .15)
                before = last(link.rows, 'PW_STICK_POWER_HW ')
                assert 'panel_awake=0 ' in before and 'gpio_out=00 ' in before, before
                if trigger == 'l':
                    link.command('l', .3)
                    link.command('Po', .1)
                    dark = last(link.rows, 'PW_STICK_POWER_HW ')
                    ignored = ('panel_awake=0 ' in dark and 'gpio_out=00 ' in dark
                               and 'open=0 ' in last(link.rows, 'PW_STICK_DEVICE_MENU '))
                    report['cases'].append({'repeat': repeat, 'input': 'dark L event',
                                            'after': dark, 'passed': ignored})
                    assert ignored, dark
                    link.command('v', .85)  # Only M begins a physical wake.
                link.collect(repeat * .037)
                start = time.monotonic()
                link.command(trigger, .9)
                link.command('DPto' if args.lcd_readback else 'Pto', .15)
                after = last(link.rows, 'PW_STICK_POWER_HW ')
                observation = next(r for r in reversed(link.rows)
                                   if r['text'] == after)
                case = {'repeat': repeat, 'input': name, 'before': before,
                        'after': after,
                        'observed_after_ms': (observation['host_monotonic'] - start) * 1000}
                case['passed'] = ('panel_awake=1 ' in after and
                                  'pwm_ready=1 ' in after and
                                  'gpio_out=04 ' in after and
                                  case['observed_after_ms'] <= 1100)
                if args.lcd_readback:
                    lcd = last(link.rows, 'PW_STICK_LCD_REG ')
                    case['lcd'] = lcd
                    match = re.search(r'power=([0-9a-f]+).*pixel=([0-9a-f]+)', lcd)
                    case['passed'] = case['passed'] and bool(match) and (
                        int(match[1], 16) & 0x54 == 0x14 and
                        int(match[2], 16) == 0x05)
                report['cases'].append(case)
                assert case['passed'], case
                if trigger == 'l':
                    assert 'open=1 ' in last(link.rows, 'PW_STICK_DEVICE_MENU ')
                    link.command('l', .3)
                    link.command('o', .15)
                    assert 'open=0 ' in last(link.rows, 'PW_STICK_DEVICE_MENU ')
        report['passed'] = True
    except Exception as exc:
        report['error'] = str(exc)
        raise
    finally:
        report['rows'] = link.rows
        link.close()
        args.output.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({k: v for k, v in report.items() if k != 'rows'}, indent=2))


if __name__ == '__main__':
    main()
