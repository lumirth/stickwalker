#!/usr/bin/env python3
"""Check repeated native move cues after an 80 ms stall on a bench build.

G loads the actual EEPROM score without navigating or editing the save. Native
BeepTick owns its shared workspace. USB remains awake; this is a tone command
lifetime and power-release check, not a microphone or battery measurement.
Restore production after use. Native sound must already be enabled.
"""
import argparse
import json
from pathlib import Path

from hardware_power_probe import Link, fields, last


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--repeats', type=int, default=20)
    args = p.parse_args()
    assert 1 <= args.repeats <= 50
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'cases': [], 'audibility_measured': False}
    link = Link(args.port)
    try:
        link.collect(3)
        last(link.rows, 'PW_STICK_READY')
        link.command('n', 1)
        link.command('s', .1)
        before = fields(last(link.rows, 'PW_STICK_SOUND '))
        timing_before = fields(last(link.rows, 'PW_STICK_SOUND_TIMING '))
        report['before'] = before
        for i in range(args.repeats):
            link.command('G', .25)
            started = fields(last(link.rows, 'PW_STICK_SOUND_STALL '))
            link.command('s', .05)
            after = fields(last(link.rows, 'PW_STICK_SOUND '))
            timing = fields(last(link.rows, 'PW_STICK_SOUND_TIMING '))
            row = {'index': i, 'start': started, 'sound': after, 'timing': timing}
            row['passed'] = (started['started'] == 1 and
                             after['tones'] == before['tones'] + i + 1 and
                             after['tone_errors'] == before['tone_errors'] and
                             after['active'] == 0 and
                             timing['measured'] == timing_before['measured'] + i + 1 and
                             timing['under20'] == timing_before['under20'] and
                             timing['shortest_us'] >= 40000)
            report['cases'].append(row)
            assert row['passed'], row
        assert after['begins'] == before['begins'] + 1, 'warm repeats must reuse I2S'
        assert after['begin_errors'] == before['begin_errors']
        assert after['power_errors'] == before['power_errors']
        link.collect(.7)
        link.command('sP', .15)
        stopped = fields(last(link.rows, 'PW_STICK_SOUND '))
        assert stopped['ready'] == 0 and stopped['codec'] == 0, stopped
        report['stopped'] = stopped
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
