#!/usr/bin/env python3
"""Compare buffered motion with frequent servicing of the same physical FIFO."""
import argparse
import csv
import os
from pathlib import Path
import subprocess
from fixtures import expedition

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('rom', type=Path)
parser.add_argument('firmware', type=Path)
parser.add_argument('flash', type=Path, help='Saved expedition flash from the send journey')
parser.add_argument('output', type=Path)
parser.add_argument('--reference-firmware', type=Path, help='Optional earlier build for acquisition-filter comparison')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
seed = args.output / 'midnight.bin'
expedition(args.flash, seed, daily=0, hourly=0, random_state=0x12345678, clock_seconds=86300)
environment = {k: v for k, v in os.environ.items() if not k.startswith('STICK_')}

def run(name, firmware=None, **options):
    output = args.output / name
    subprocess.run([root / 'e2e/run', args.rom, firmware or args.firmware, seed, 'pocket-walk', output],
                   env=environment | options, check=True)
    with (output / 'motion.csv').open() as file:
        return {k: int(v) for k, v in next(csv.DictReader(file)).items()}

# Early hardware timer wakes give the production game the same timestamped
# samples promptly. Only service timing changes; no second pedometer algorithm.
reference = run('frequent', STICK_MOTION_REFERENCE='1', STICK_POWER_BASELINE='1')
for ppm in (0, -15000, 15000):
    actual = run(f'buffered-{ppm}', STICK_IMU_PPM=str(ppm))
    for field in ('steps', 'daily', 'yesterday'):
        assert abs(actual[field] - reference[field]) <= 2, (ppm, field, reference, actual)
    print(f'{ppm:+} ppm: {actual}; frequent-service reference: {reference}', flush=True)

# Changing cadence exercises complete FFT windows and their rhythm history.
# An earlier build additionally compares FIFO interpolation with live sampling.
reference = run('varied-reference', firmware=args.reference_firmware,
                STICK_WALK_TRACE='varied', STICK_MOTION_COMPARE='1',
                STICK_MOTION_REFERENCE='1', STICK_POWER_BASELINE='1')
actual = run('varied-buffered', STICK_WALK_TRACE='varied', STICK_MOTION_COMPARE='1')
for field in ('steps', 'daily', 'yesterday'):
    assert abs(actual[field] - reference[field]) <= 2, (field, reference, actual)
print(f'Changing cadence: {actual}; reference: {reference}', flush=True)
