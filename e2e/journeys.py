#!/usr/bin/env python3
"""Run the StickS3 firmware with local retail EEPROMs and the simulated HGSS game."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from tempfile import TemporaryDirectory
from fixtures import expedition

ROOT = Path(__file__).resolve().parents[1]
from storage_reader import decode

STATUS = 0xED
COURSE = slice(0x8F00, 0x8F00 + 10430)
PAIRING = slice(STATUS + 12, STATUS + 56)


def flags(eeprom: bytes) -> int:
    return eeprom[STATUS + 91] & 3


def steps(eeprom: bytes) -> int:
    return int.from_bytes(eeprom[0x156:0x15A], "big")


def run(args, output: Path) -> None:
    subprocess.run([sys.executable, ROOT / "pokewalker/port/check_upstream.py"], check=True)
    subprocess.run([sys.executable, ROOT / "e2e/infrared.py"], check=True)
    subprocess.run([sys.executable, ROOT / "e2e/storage.py"], check=True)
    ready = args.empty.read_bytes()
    retail = args.course.read_bytes()
    if len(ready) != 65536 or len(retail) != 65536 or flags(ready) != 1 or flags(retail) != 3:
        raise ValueError("Use a registered empty walker and an active retail course")

    subprocess.run([ROOT / "e2e/run", args.rom, args.firmware, args.empty, "clock", output / "clock"],
                   cwd=ROOT, check=True)

    def boot(name, journey, seed, *, trace=None, carry=False, **options):
        directory = output / name
        directory.mkdir(exist_ok=True)
        command = [ROOT / "e2e/run", args.rom, args.firmware, seed, journey, directory]
        if trace:
            command.append(trace)
        environment = {key: value for key, value in os.environ.items() if not key.startswith("STICK_")}
        if carry:
            options["STICK_KEEP_FLASH"] = "1"
        subprocess.run(command, cwd=ROOT, env=environment | {"STICK_TIMING": args.timing} | options, check=True)
        print(name, flush=True)
        if carry:
            partition = (directory / "flash.bin").read_bytes()[0x610000:]
        else:
            partition = (directory / "walker2.bin").read_bytes()
            (directory / "walker2.bin").unlink()
        image, runtime = decode(partition)
        if journey in ("first-boot", "send", "return", "pair", "gameplay", "inventory") and not any(key in options for key in (
                "STICK_TEAR_COMMIT", "STICK_UNPLUG_ON_SEND", "STICK_DROP_PULSE_IN_REPLY")):
            assert image == (directory / "live-eeprom.bin").read_bytes()
        return image, runtime

    def send(name, seed=args.empty, *, carry=False, **options):
        return boot(name, "send", seed, carry=carry, STICK_COURSE_EEPROM=str(args.course),
                    STICK_PEER_EEPROM=str(args.empty), **options)

    def sent_course(eeprom):
        assert flags(eeprom) == 3 and eeprom[COURSE] == retail[COURSE]
        assert eeprom[PAIRING] == ready[PAIRING]

    def previous_save(eeprom):
        assert flags(eeprom) == 1 and eeprom[COURSE] == ready[COURSE]
        assert eeprom[PAIRING] == ready[PAIRING]

    def stage(*journeys):
        """Run independent journeys at once; the next stage starts when all pass."""
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = [pool.submit(journey) for journey in journeys]
            for future in as_completed(futures):
                future.result()
        return [future.result() for future in futures]

    erased = output / "empty.eeprom"
    erased.write_bytes(b"\xff" * 65536)

    def first_boot():
        fresh, _ = boot("first-boot", "first-boot", erased)
        assert fresh[:8] == b"nintendo" and flags(fresh) == 0
        identity = fresh[0x83:0xAB]
        assert identity[-6:] == bytes.fromhex('020500000001')
        assert identity == fresh[0x183:0x1AB] == fresh[STATUS + 16:STATUS + 56]
        return identity

    def second_identity():
        other, _ = boot("second-identity", "first-boot", erased, STICK_MAC="02:05:00:00:00:02")
        assert other[0x83:0xAB] == other[0x183:0x1AB] == other[STATUS + 16:STATUS + 56]
        return other[0x83:0xAB]

    def imported_identity():
        imported, _ = boot("imported-identity", "first-boot", args.empty, STICK_MAC="02:05:00:00:00:02")
        assert imported[0x83:0xAB] == ready[0x83:0xAB]
        assert imported[0x183:0x1AB] == ready[0x183:0x1AB]
        assert imported[PAIRING] == ready[PAIRING]

    def faulted(name, fault):
        def journey():
            recovered, _ = send(name, carry=name == "cut-during-publication", **fault)
            previous_save(recovered)
        return journey

    def checked_send(name, **options):
        def journey():
            sent_course(send(name, **options)[0])
        return journey

    sent_flash = output / "send/flash.bin"
    identity, other, _, (sent, clock), *_ = stage(
        first_boot, second_identity, imported_identity,
        lambda: send("send", carry=True, STICK_IR_PATH="weak-low-trip", STICK_POWER="expansion"),
        faulted("cut-during-upload", {"STICK_TEAR_COMMIT": "upload"}),
        faulted("cut-during-publication", {"STICK_TEAR_COMMIT": "publish"}),
        faulted("corrupted-send", {"STICK_DROP_PULSE_IN_REPLY": "30"}),
        checked_send("unplugged-send", STICK_UNPLUG_ON_SEND="1"),
        checked_send("lost-commit-reply", STICK_LOSE_COMMIT_REPLY="1", STICK_IR_PATH="slow-pad", STICK_FAST_REPLY="1"),
        checked_send("usb-insertion", STICK_PLUG_ON_SEND="1", STICK_POWER="battery", STICK_IR_PATH="bright"),
    )
    assert other != identity
    sent_course(sent)

    def pocket_walk():
        walked, walked_clock = boot("pocket-walk", "pocket-walk", sent_flash, carry=True)
        assert 150 <= steps(walked) - steps(sent) <= 230
        assert walked[COURSE] == sent[COURSE]
        assert int.from_bytes(walked_clock[:8], "little") > int.from_bytes(clock[:8], "little")
        print(f"Pocket walk credited {steps(walked) - steps(sent)} steps", flush=True)
        trainer = output / "trainer.eeprom"
        trainer.write_bytes(walked)
        returned, _ = boot("return", "return", output / "pocket-walk/flash.bin",
                           STICK_PEER_EEPROM=str(trainer), STICK_POWER="battery", STICK_IR_PATH="weak")
        assert flags(returned) == 1 and returned[PAIRING] == sent[PAIRING]
        reply = (output / "return/walker.txt").read_text().splitlines()
        rental = bytes(byte ^ 0xAA for byte in bytes.fromhex(reply[4]))
        assert rental[0] == 0x0E and rental[8:24] == retail[COURSE][:16]
        assert steps(returned) == steps(walked)

    def muted_input():
        muted = bytearray(sent)
        for at in (0x156, 0x256):
            muted[at + 23] &= ~6
            muted[at + 24] = (1 + sum(muted[at:at + 24])) & 255
        muted_path = output / "muted.eeprom"
        muted_path.write_bytes(muted)
        boot("muted-input", "muted-input", muted_path)

    def plain(name):
        return lambda: boot(name, name, sent_flash)

    def depleted():
        image, _ = boot("depleted", "depleted", sent_flash)
        assert image[PAIRING] == sent[PAIRING] and image[COURSE] == sent[COURSE]

    def recovered(name):
        def journey():
            image, _ = boot(name, name, sent_flash)
            assert flags(image) == 3 and image[COURSE] == sent[COURSE]
            assert image[PAIRING] == sent[PAIRING] and steps(image) == steps(sent)
        return journey

    def gameplay():
        # Start from a saved expedition with earned history; the separate pocket
        # journey exercises the sensor path. Keep the retail course's nonzero gates.
        gameplay_seed = output / "gameplay-seed.bin"
        threshold = int.from_bytes(sent[0x8f00 + 130:0x8f00 + 132], "little")
        assert threshold > 0
        expedition(sent_flash, gameplay_seed, daily=threshold + 1500, hourly=2000,
                   random_state=0x12345678)
        played, _ = boot("gameplay", "gameplay", gameplay_seed, carry=True)
        rebooted, _ = boot("inventory-reboot", "inventory", output / "gameplay/flash.bin", carry=True)
        assert played[0xce8c:0xcecc] == rebooted[0xce8c:0xcecc]
        trainer = output / "gameplay-trainer.eeprom"
        trainer.write_bytes(played)
        finished, _ = boot("expedition-return", "return", output / "gameplay/flash.bin",
                           STICK_PEER_EEPROM=str(trainer))
        assert flags(finished) == 1 and finished[PAIRING] == sent[PAIRING]

    def peers():
        first, second = output / "peer-a.bin", output / "peer-b.bin"
        peer_identity = bytearray(sent[0x83:0xab])
        peer_identity[-1] ^= 1
        expedition(sent_flash, first, daily=4500, hourly=4000, random_state=0x12345678, midnight=True)
        expedition(sent_flash, second, daily=3500, hourly=3000, random_state=0x71654321,
                   identity=peer_identity, midnight=True)
        subprocess.run([ROOT / "e2e/run", args.rom, args.firmware, first, "walkers", output / "peers", second],
                       cwd=ROOT, env=os.environ | {"STICK_TIMING": args.timing}, check=True)
        print("peers", flush=True)

    stage(
        pocket_walk, gameplay, peers, muted_input, depleted,
        checked_send("retry", seed=output / "cut-during-publication/flash.bin", STICK_IR_PATH="wide-pad"),
        *(plain(name) for name in ("connect-noise", "wake-audio", "controls-and-settings", "settings-focus",
                                   "stationary", "sleep-wake", "low-battery-audio", "sound-and-colors")),
        *(recovered(name) for name in ("power-recovery", "watchdog-loop", "usb-recovery")),
    )

    if args.pair:
        before, trace, prediction = args.pair
        paired, _ = boot("pair", "pair", before, trace=trace)
        expected = prediction.read_bytes()
        assert flags(paired) & 1
        assert paired[0x280:0x8ED0] == expected[0x280:0x8ED0]
        assert paired[COURSE] == expected[0xD700:0xD700 + 10430]
        assert paired[0xCC00:0xCC00 + 548] == expected[0xD480:0xD480 + 548]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path, help="ESP32-S3 mask ROM ELF")
    parser.add_argument("firmware", type=Path, help="directory holding the built Stick firmware")
    parser.add_argument("empty", type=Path, help="registered, empty Pokéwalker EEPROM dump")
    parser.add_argument("course", type=Path, help="original-format EEPROM dump holding the Pokémon to send")
    parser.add_argument("--pair", type=Path, nargs=3, metavar=("BEFORE", "TRACE", "PREDICTED"))
    parser.add_argument("--timing", choices=("typical", "maximum"), default="typical",
                        help="GD25Q64E program and erase timing profile")
    parser.add_argument("--output", type=Path, help="keep screenshots and output here")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1,
                        help="journeys to simulate at once (default: one per CPU core)")
    args = parser.parse_args()
    if not __debug__:
        parser.error("Run the journeys with Python assertions enabled")
    for name in ("rom", "firmware", "empty", "course"):
        setattr(args, name, getattr(args, name).resolve())
    if args.pair:
        args.pair = [path.resolve() for path in args.pair]
    if args.output:
        args.output = args.output.resolve()
        args.output.mkdir(parents=True, exist_ok=True)
        run(args, args.output)
    else:
        with TemporaryDirectory(prefix="stickwalker-") as directory:
            run(args, Path(directory))
