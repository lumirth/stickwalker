#!/usr/bin/env python3
"""Check cold/warm native move cues using the board's local acoustic self-test.

K returns codec digital feedback in the left channel and microphone metrics
in the right channel. No microphone audio is written to disk or sent to the
host. At 22.05 kHz/512 frames the 712 Hz native cue lands in bin 16/17;
the speaker's dominant fifth harmonic lands in bin 82/83. An unprimed ADC
cannot pass: nonzero/energetic startup transients alone are insufficient.
This is a USB acoustic/lifecycle check, not a battery discharge measurement.
"""
import argparse
import json
from pathlib import Path

from hardware_power_probe import Link, fields, last


def assess(rows):
    captures = [r['text'] for r in rows if r['text'].startswith('PW_STICK_SOUND_CAPTURE ')]
    spectra = [r['text'] for r in rows if r['text'].startswith('PW_STICK_SOUND_SPECTRUM ')]
    cases = []
    for index, (capture, spectrum) in enumerate(zip(captures, spectra)):
        header, values = capture.split('bins=')
        status, frequency = fields(header), fields(spectrum)
        bins = [list(map(int, b.split(':'))) for b in values.split(',') if b]
        note = [b for b in bins if b[1] > 1000 and b[3] >= 120]
        passed = (status['started'] == 1 and status['ok'] == 1 and status['error'] == 0 and
                  frequency['frames'] == 512 and frequency['left_bin'] in (16, 17) and
                  frequency['right_bin'] in (82, 83) and sum(b[0] for b in note) >= 512)
        cases.append({'index': index, 'cold': index % 2 == 0, 'passed': passed,
                      'status': status, 'spectrum': frequency,
                      'band_samples': sum(b[0] for b in note),
                      'peak_band_amplitude': max(b[3] for b in bins)})
    return cases


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port')
    p.add_argument('--replay', type=Path)
    p.add_argument('--pairs', type=int, default=12)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    assert bool(args.port) != bool(args.replay)
    assert 1 <= args.pairs <= 30
    report = {'passed': False, 'rows': [], 'cases': []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.replay:
        report['rows'] = json.loads(args.replay.read_text())
        report['cases'] = assess(report['rows'])
        report['passed'] = bool(report['cases']) and all(c['passed'] for c in report['cases'])
    else:
        link = Link(args.port)
        try:
            link.collect(3)
            last(link.rows, 'PW_STICK_READY')
            link.command('n', 1)
            for pair in range(args.pairs):
                link.command('sP', .1)
                state = fields(last(link.rows, 'PW_STICK_SOUND '))
                hardware = fields(last(link.rows, 'PW_STICK_POWER_HW '))
                assert state['ready'] == 0 and state['codec'] == 0 and hardware['panel_awake'] == 0
                for _ in range(2):
                    link.command('K', .45)
                    report['cases'] = assess(link.rows)
                    assert len(report['cases']) == pair * 2 + _ + 1
                    assert report['cases'][-1]['passed'], report['cases'][-1]
                    assert not any('Guru Meditation' in r['text'] for r in link.rows)
                link.collect(.75)
            link.command('sP', .1)
            report['stopped'] = fields(last(link.rows, 'PW_STICK_SOUND '))
            assert report['stopped']['ready'] == 0 and report['stopped']['codec'] == 0
            assert report['stopped']['begin_errors'] == 0 and report['stopped']['power_errors'] == 0
            assert report['stopped']['tone_errors'] == 0
            report['passed'] = True
        except Exception as exc:
            report['error'] = str(exc)
        finally:
            report['rows'] = link.rows
            link.close()
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'rows'}, indent=2))
    raise SystemExit(0 if report['passed'] else 1)


if __name__ == '__main__':
    main()
