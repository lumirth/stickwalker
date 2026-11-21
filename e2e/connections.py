#!/usr/bin/env python3
"""Four connection stories, using the real HGSS engine on an independent clock."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
from storage_reader import decode

parser = argparse.ArgumentParser(description=__doc__)
for name in ('rom', 'firmware', 'empty', 'course'):
    parser.add_argument(name, type=lambda p: Path(p).resolve())
parser.add_argument('--seed', type=int, default=1)
parser.add_argument('--story', action='append', choices=('hot', 'delayed', 'glitch', 'retry'))
parser.add_argument('--path', action='append', choices=('nominal', 'weak', 'weak-low-trip', 'wide-pad', 'slow-pad', 'bright'))
parser.add_argument('--jobs', type=int, default=os.cpu_count() or 1,
                    help='scenarios to simulate at once (default: one per CPU core)')
args = parser.parse_args()
if not __debug__:
    parser.error('Run with Python assertions enabled')
if not 0 < args.seed <= 0xffffffff:
    parser.error('Seed must be between 1 and 4294967295')

# By default, hot startup runs on every hard optical path and each recovery
# story on one of them, so every story and path is covered without crossing them.
if args.story or args.path:
    cases = [(path, story) for path in args.path or ['weak', 'slow-pad', 'bright']
             for story in args.story or ['hot', 'delayed', 'glitch', 'retry']]
else:
    cases = [('weak', 'hot'), ('slow-pad', 'hot'), ('bright', 'hot'),
             ('weak', 'delayed'), ('slow-pad', 'glitch'), ('bright', 'retry')]
output = threading.Lock()
finished = 0


def report(message, stream=sys.stdout):
    global finished
    with output:
        finished += 1
        print(f'[{finished}/{len(cases) * 2}] {message}', file=stream, flush=True)


def scenario(work, path, story):
    """Send, then return from the flash the send produced. Returns whether both passed."""
    seed = args.empty
    trainer = args.empty
    for index, direction in enumerate(('send', 'return')):
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
                    direction, out], env=environment, cwd=ROOT,
                    stdout=log, stderr=subprocess.STDOUT, check=True)
            image, _ = decode((out / 'flash.bin').read_bytes()[0x610000:])
            assert image == (out / 'live-eeprom.bin').read_bytes(), 'Persisted EEPROM differs from game RAM'
            assert image[0x148] & 3 == (3 if direction == 'send' else 1), 'Wrong expedition state'
            assert image[0xf9:0x125] == args.empty.read_bytes()[0xf9:0x125], 'Pairing changed'
            if direction == 'send':
                assert image[0x8f00:0x8f00+10430] == args.course.read_bytes()[0x8f00:0x8f00+10430], 'Wrong course'
            # A cold boot after one return checks the IR-written save survives it.
            if direction == 'return' and (path, story) == cases[0]:
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
            report(f'{story} {direction}: pass ({path}, seed {args.seed})')
        except Exception as error:
            failure = ROOT / 'build' / f'connection-failure-{story}-{direction}-{path}-{args.seed}'
            if failure.exists(): shutil.rmtree(failure)
            shutil.copytree(out, failure)
            log = (out / 'run.log').read_text()[-3000:] if (out / 'run.log').exists() else ''
            report(f'FAIL {story} {direction} ({path}, seed {args.seed}): {error or type(error).__name__}\n'
                   f'  trace: {failure}\n{log}', sys.stderr)
            if index == 0:
                report(f'skip {story} return ({path}, seed {args.seed}): needs the send', sys.stderr)
            return False
    return True


with tempfile.TemporaryDirectory(prefix='stick-connections-') as temporary:
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = {pool.submit(scenario, Path(temporary), path, story): (path, story) for path, story in cases}
        failed = [futures[future] for future in as_completed(futures) if not future.result()]
if failed:
    sys.exit('Failed: ' + ', '.join(f'{story} on {path}' for path, story in failed))
print(f'All {len(cases)} scenarios passed (seed {args.seed}).')
