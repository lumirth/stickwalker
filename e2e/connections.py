#!/usr/bin/env python3
"""Four connection stories, using the real HGSS engine on an independent clock."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
from storage_reader import decode

parser = argparse.ArgumentParser(description=__doc__)
for name in ('rom', 'firmware', 'empty', 'course', 'bench'):
    parser.add_argument(name, type=lambda p: Path(p).resolve())
parser.add_argument('--seed', type=int, default=1)
parser.add_argument('--story', choices=('hot', 'delayed', 'glitch', 'retry'))
parser.add_argument('--path', choices=('nominal', 'weak', 'weak-low-trip', 'wide-pad', 'slow-pad', 'bright'))
args = parser.parse_args()
if not __debug__:
    parser.error('Run with Python assertions enabled')
if not 0 < args.seed <= 0xffffffff:
    parser.error('Seed must be between 1 and 4294967295')

with tempfile.TemporaryDirectory(prefix='stick-connections-') as temporary:
    work = Path(temporary)
    peer = work / 'peer'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        *['-I' + str(args.bench / p) for p in ('include', 'include/hgss', 'vendor/hgss')],
        *[args.bench / 'source' / p for p in ('hgss_irc.c', 'hgss_phc.c', 'hgss_peer.c')],
        ROOT / 'e2e/peer.c', '-o', peer], check=True)
    stories = [args.story] if args.story else ['hot', 'delayed', 'glitch', 'retry']
    paths = [args.path] if args.path else ['weak', 'slow-pad', 'bright']
    for path, story in ((path, story) for path in paths for story in stories):
        seed = args.empty
        trainer = args.empty
        for direction in ('send', 'return'):
            out = work / (path + '-' + story + '-' + direction)
            out.mkdir()
            environment = {k: v for k, v in os.environ.items() if not k.startswith('STICK_')}
            environment.update(STICK_CONNECTION=story, STICK_SEED=str(args.seed),
                STICK_TIMING='maximum', STICK_KEEP_FLASH='1',
                STICK_IR_PATH=path,
                STICK_PEER_EEPROM=str(trainer), STICK_COURSE_EEPROM=str(args.course))
            if story == 'hot': environment['STICK_FAST_REPLY'] = '1'
            try:
                with (out / 'run.log').open('w') as log:
                    subprocess.run([ROOT / 'e2e/run', args.rom, args.firmware, seed,
                        direction, out, peer], env=environment, cwd=ROOT,
                        stdout=log, stderr=subprocess.STDOUT, check=True)
                image, _ = decode((out / 'flash.bin').read_bytes()[0x610000:])
                assert image == (out / 'live-eeprom.bin').read_bytes(), 'Persisted EEPROM differs from game RAM'
                assert image[0x148] & 3 == (3 if direction == 'send' else 1), 'Wrong expedition state'
                assert image[0xf9:0x125] == args.empty.read_bytes()[0xf9:0x125], 'Pairing changed'
                if direction == 'send':
                    assert image[0x8f00:0x8f00+10430] == args.course.read_bytes()[0x8f00:0x8f00+10430], 'Wrong course'
                if direction == 'return':
                    reboot = out / 'reboot'
                    reboot.mkdir()
                    reboot_env = {k: v for k, v in environment.items() if not k.startswith('STICK_')}
                    reboot_env.update(STICK_TIMING='maximum', STICK_KEEP_FLASH='1')
                    with (reboot / 'run.log').open('w') as log:
                        subprocess.run([ROOT / 'e2e/run', args.rom, args.firmware,
                            out / 'flash.bin', 'first-boot', reboot], env=reboot_env,
                            cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
                    assert (reboot / 'live-eeprom.bin').read_bytes() == image, 'Return changed after reboot'
                seed = out / 'flash.bin'
                trainer = out / 'live-eeprom.bin'
                print(f'{story} {direction}: pass ({environment["STICK_IR_PATH"]}, seed {args.seed})', flush=True)
            except Exception:
                failure = ROOT / 'build' / f'connection-failure-{story}-{direction}-{environment["STICK_IR_PATH"]}-{args.seed}'
                if failure.exists(): shutil.rmtree(failure)
                shutil.copytree(out, failure)
                print(f'FAIL {story} {direction}, seed {args.seed}: {failure}', file=sys.stderr)
                if (out / 'run.log').exists(): print((out / 'run.log').read_text()[-3000:], file=sys.stderr)
                raise
