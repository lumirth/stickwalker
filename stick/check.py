#!/usr/bin/env python3
"""Build and exercise production firmware on a simulated Stick S3."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import zlib
from array import array
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "stick/simulation"))
from bootstrap import REVISION, prepare  # noqa: E402
from fixture import course, storage  # noqa: E402

PUT_SCENARIOS = (
    "peer-transfer",
    "peer-transfer-compressed",
    "peer-noise",
    "peer-prefix-byte",
    "peer-slow",
    "peer-initial",
    "peer-rc-low",
    "peer-rc-high",
    "peer-rc-gap",
    "peer-immediate",
    "peer-deadline",
    "peer-keepalive",
)
EXCHANGE_SCENARIOS = PUT_SCENARIOS + ("peer-return", "peer-play")
OPTICAL_SCENARIOS = EXCHANGE_SCENARIOS + ("peer-malformed", "peer-keepalive-only")

SCENARIOS = (
    "settings",
    "connect",
    "diode",
    "diode-peer",
    "diode-ready",
    "diode-presence",
    "diode-timeout",
    "diode-stall",
    "margin-power",
    "margin-sleep",
    "connection-details",
    "diode-correction",
    "controls",
    "layouts",
    "wake",
    "battery",
    "recovery-stale",
    "recovery-journal",
    "recovery-truncated",
    "peer",
    "peer-storage",
    *OPTICAL_SCENARIOS,
)

SCENARIO_COST = {
    "peer-initial": 300,
    "peer-prefix-byte": 185,
    "peer-malformed": 180,
    "peer-noise": 150,
    "peer-transfer": 140,
    "peer-transfer-compressed": 140,
    "peer-slow": 130,
    "peer-rc-low": 150,
    "peer-rc-high": 150,
    "peer-rc-gap": 160,
    "peer-immediate": 140,
    "peer-deadline": 150,
    "peer-keepalive": 150,
    "peer-keepalive-only": 50,
    "peer-return": 75,
    "peer-play": 70,
    "margin-power": 60,
    "connection-details": 45,
}


def shard_scenarios(index: int, count: int) -> tuple[str, ...]:
    if not 1 <= index <= count <= len(SCENARIOS):
        raise ValueError(f"Shard must be between 1 and {len(SCENARIOS)} and not exceed its count")
    loads = [0] * count
    assignments = {}
    for scenario in sorted(SCENARIOS, key=lambda name: -SCENARIO_COST.get(name, 30)):
        shard = min(range(count), key=loads.__getitem__)
        assignments[scenario] = shard
        loads[shard] += SCENARIO_COST.get(scenario, 30)
    return tuple(scenario for scenario in SCENARIOS if assignments[scenario] == index - 1)


def parse_shard(value: str) -> tuple[int, int]:
    try:
        index, count = (int(part) for part in value.split("/"))
        shard_scenarios(index, count)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected a valid shard as INDEX/COUNT") from error
    return index, count


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expect_appearance(path: Path, light: bool) -> None:
    brightness = statistics.median(Image.open(path).convert("L").tobytes())
    if (brightness >= 128) != light:
        raise AssertionError(f"{path}: selected appearance does not match the visible background")


def expect_diode(path: Path, state: str | None) -> None:
    image = Image.open(path).rotate(90, expand=True).convert("L")
    bits = bytes(
        image.getpixel((4 + col, 4 + row)) >= 128 for row in range(16) for col in range(16)
    )
    hashes = {
        "missing": "9dd801c7cf41981ecdab05068b9aafad57b6991fd9086bc5b3d115556ae61325",
        "connected": "59df9fbf1e7334530d2c4dfdb0f6abd4aea4de8f585b714e208583cacc793cbd",
        "unknown": "8fd5e17294d62ee5ecb2a7d38defb96b6b60c8d3457c87b3c10af29095d79b52",
        "blocked": "a17ceb5e48e4b17f1e1222bb8adbf65633ba59e7fdb09714e908078bccd56ffb",
        "receiving": "67834819e472250764042cf3d306cb77f304e21c43b7030f3adc0a791d9a19d5",
    }
    if state is None:
        assert not any(bits), f"{path}: diode feedback did not clear"
    else:
        assert hashlib.sha256(bits).hexdigest() == hashes[state], f"{path}: expected {state}"


def expect_battery(path: Path, level: int | None, usb: bool = False) -> None:
    image = Image.open(path).rotate(90, expand=True).convert("L")
    rows = [0] * 16
    if level is not None:
        rows[4] = rows[11] = 0x3FFE
        for row in range(5, 11):
            rows[row] = 0x2002 | (0xC000 if 6 <= row <= 9 else 0)
            for bar in range(level):
                rows[row] |= 3 << (3 + bar * 2)
    if usb:
        rows[:3] = [0x0140, 0x03E0, 0x0080]
    observed = bytes(image.getpixel((4 + x, 115 + y)) >= 128 for y in range(16) for x in range(16))
    expected = bytes(bool(rows[y] & (1 << x)) for y in range(16) for x in range(16))
    assert observed == expected, f"{path}: expected battery level={level}, USB={usb}"


def render_margin_atlas(out: Path) -> None:
    sources = [
        ("Not detected", "diode-presence/missing.png"),
        ("Reverse", "diode/reversed.png"),
        ("Detected", "diode-ready/ready-0.png"),
        ("Inconclusive", "connect/no-peer.png"),
        ("Blocked", "diode/unknown.png"),
        ("Packet heard", "connection-details/failed.png"),
    ]
    if not all((out / name).exists() for _, name in sources):
        return
    atlas = Image.new("RGB", (1200, 220), "#ddd9cf")
    draw = ImageDraw.Draw(atlas)
    for index, (label, name) in enumerate(sources):
        path = out / name
        if name.startswith("diode-ready/"):
            path = next(
                candidate
                for candidate in sorted((out / "diode-ready").glob("ready-*.png"))
                if any(
                    value >= 128
                    for value in Image.open(candidate)
                    .rotate(90, expand=True)
                    .crop((4, 4, 20, 20))
                    .convert("L")
                    .tobytes()
                )
            )
        tile = Image.open(path).rotate(90, expand=True).crop((4, 4, 20, 20))
        atlas.paste(tile.resize((128, 128), Image.Resampling.NEAREST), (index * 200 + 20, 10))
        atlas.paste(tile, (index * 200 + 20, 155))
        draw.text((index * 200 + 20, 185), label, fill="#152120")
    atlas.save(out / "margin-icons.png")


def diode_report(path: Path, *, require_released: bool = True) -> dict[str, str]:
    report = dict(line.split("=", 1) for line in path.read_text().splitlines())
    assert report["reboots"] == "0", f"{path}: diode check rebooted"
    if require_released:
        assert report["output_enabled"] == "false", f"{path}: GPIO5 output remains enabled"
    assert report["touch_timer"] == "false", f"{path}: continuous touch scans enabled"
    assert report["touch_mask"] == "0", f"{path}: touch controller left enabled"
    return report


def optical_frames(path: Path) -> list[bytes]:
    frames: list[list[tuple[int, bool]]] = []
    for line in path.read_text().splitlines():
        fields = line.split()
        if fields[0] == "frame":
            frames.append([])
        else:
            frames[-1].append((int(fields[0]), fields[1] == "1"))
    decoded = []
    # SIR represents UART zero with a short light pulse; an unlit cell is one.
    # Decode the external signal using the 115200-baud, 8N1 link specification.
    cell = 240_000_000 / 115200
    for pulses in frames:
        elapsed = 0
        illuminated: list[tuple[int, int]] = []
        for duration, high in pulses:
            if high:
                illuminated.append((elapsed, elapsed + duration))
            elapsed += duration
        count = round(elapsed / cell)
        if count == 0 or count % 10 or abs(elapsed - count * cell) > count * 3:
            raise AssertionError(f"{path}: invalid UART frame duration {elapsed}")
        bits = [True] * count
        for start, end in illuminated:
            index = round(start / cell)
            if not 0 <= index < count or abs(start - index * cell) > cell * 0.05:
                raise AssertionError(f"{path}: light pulse falls outside a UART cell")
            if not cell * 0.1 <= end - start <= cell * 0.3:
                raise AssertionError(f"{path}: light pulse has invalid SIR duration")
            bits[index] = False
        payload = bytearray()
        for start in range(0, count, 10):
            if bits[start] or not bits[start + 9]:
                raise AssertionError(f"{path}: invalid UART start/stop cell")
            payload.append(sum(int(bits[start + 1 + bit]) << bit for bit in range(8)))
        decoded.append(bytes(byte ^ 0xAA for byte in payload))
    if not decoded:
        raise AssertionError(f"{path}: Connect emitted no infrared data")
    return decoded


OVERLAY_HEADER = (0, 0, 240, 24)
NATIVE_HEADER = (24, 3, 216, 25)


def screen_region(
    path: Path, box: tuple[int, int, int, int], rotation: int = 90, invert: bool = False
) -> bytes:
    image = Image.open(path).rotate(rotation, expand=True).crop(box)
    if invert:
        image = ImageOps.invert(image.convert("RGB"))
    return image.tobytes()


def expect_overlay(folder: Path, path: Path) -> None:
    if screen_region(path, OVERLAY_HEADER) != screen_region(
        folder / "defaults.png", OVERLAY_HEADER
    ):
        raise AssertionError(f"{path}: settings overlay did not return")


def expect_reboot(folder: Path) -> None:
    for line in (folder / "run.log").read_text().splitlines():
        if line.startswith("cycles="):
            counters = dict(field.split("=", 1) for field in line.split())
            if int(counters["reboots"]) > 0:
                return
    raise AssertionError(f"{folder}: expected a simulated reboot")


def expect_cursor(path: Path, x: int, y: int) -> None:
    image = Image.open(path).rotate(90, expand=True).convert("L")
    for row in range(8):
        for column in range(8):
            lit = image.getpixel((24 + (x + column) * 2, 3 + (y + row) * 2)) >= 128
            if lit != (2 <= row < 6):
                raise AssertionError(f"{path}: native editor cursor is not at ({x}, {y})")


def expect_tones(path: Path, count: int) -> list[dict]:
    samples = array("h", path.read_bytes())
    if sys.byteorder != "little":
        samples.byteswap()
    rate = int(path.with_name(path.name + ".rate").read_text())
    active = [i for i, value in enumerate(samples) if abs(value) > 100]
    spans: list[list[int]] = []
    for index in active:
        if not spans or index - spans[-1][1] > rate * 0.005:
            spans.append([index, index])
        else:
            spans[-1][1] = index
    if len(spans) != count:
        raise AssertionError(f"{path}: expected {count} distinct synthetic notes, got {len(spans)}")
    observations = []
    expected_frequency = 32768 / 78
    expected_duration = (int((81920 * 6 / 120 - 320) / 77) - 1) * 78 / 32768
    for start, end in spans:
        duration = (end - start + 1) / rate
        crossings = [i for i in range(start + 1, end + 1) if samples[i - 1] <= 0 < samples[i]]
        if len(crossings) < 2:
            raise AssertionError(f"{path}: note has no complete measured period")
        cycle_samples = crossings[-1] - crossings[0]
        frequency = (len(crossings) - 1) * rate / cycle_samples
        duration_limit = 2 * 4 * 64 / rate + 0.001 + 2 / rate
        if abs(duration - expected_duration) > duration_limit:
            raise AssertionError(
                f"{path}: note duration {duration:.4f}s differs from native {expected_duration:.4f}s"
            )
        expected_samples = (len(crossings) - 1) * rate / expected_frequency
        if abs(cycle_samples - expected_samples) > 2:
            raise AssertionError(
                f"{path}: note frequency {frequency:.2f} differs from native {expected_frequency:.2f} Hz"
            )
        observations.append({"seconds": duration, "frequency_hz": frequency})
    return observations


def saved_eeprom(folder: Path, offset: int, size: int) -> bytes:
    raw = (folder / "flash.bin").read_bytes()[offset : offset + size]
    checkpoints = []
    for base in (0, 0x11000):
        magic, generation, sequence, length, crc, header_crc, marker = struct.unpack_from(
            "<7I", raw, base
        )
        if marker != 0x54494D43:
            continue
        payload = raw[base + 28 : base + 28 + length]
        if (
            magic != 0x53574531
            or length != 65536
            or zlib.crc32(raw[base : base + 20]) != header_crc
            or zlib.crc32(payload) != crc
        ):
            raise AssertionError("Invalid committed EEPROM checkpoint")
        checkpoints.append((generation, sequence, payload))
    if not checkpoints:
        raise AssertionError("No committed EEPROM checkpoint")
    generation, sequence, data = max(checkpoints)
    payload = bytearray(data)
    cursor = 0x22000
    while cursor < 0x62000:
        magic, version, serial, length, crc, header_crc, marker = struct.unpack_from(
            "<7I", raw, cursor
        )
        if marker != 0x54494D43:
            break
        append = raw[cursor + 28 : cursor + 28 + length]
        if (
            magic != 0x53574A31
            or not 0 < length <= 4096
            or zlib.crc32(raw[cursor : cursor + 20]) != header_crc
            or zlib.crc32(append) != crc
        ):
            raise AssertionError("Invalid committed EEPROM journal record")
        if serial > sequence:
            if version != generation or serial != sequence + 1:
                raise AssertionError("EEPROM journal skipped a sequence")
            position = 0
            while position < len(append):
                address, count = struct.unpack_from("<HH", append, position)
                position += 4
                if address + count > 65536 or position + count > len(append):
                    raise AssertionError("EEPROM journal patch exceeds bounds")
                payload[address : address + count] = append[position : position + count]
                position += count
            sequence = serial
        cursor += (28 + length + 255) & ~255
    return bytes(payload)


def expect_saved_settings(folder: Path, offset: int, size: int) -> None:
    payload = saved_eeprom(folder, offset, size)
    for address in (0x156, 0x256):
        record = payload[address : address + 25]
        if record[24] != (1 + sum(record[:24])) & 255 or record[23] != (5 << 3):
            raise AssertionError(
                "Native volume=0/contrast=5 did not survive save and reset in both mirrors"
            )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "stick/.build/e2e-firmware")
    parser.add_argument(
        "--use-build", action="store_true", help="use a matching retained production build"
    )
    parser.add_argument(
        "--use-prepared-simulator", action="store_true", help="use a verified prepared simulator"
    )
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--config-file", type=Path)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--scenario", action="append", choices=SCENARIOS)
    selection.add_argument("--shard", type=parse_shard, metavar="INDEX/COUNT")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "stick/.build/e2e-results")
    args = parser.parse_args()
    os.chdir(ROOT)
    build = args.build_dir.resolve()
    if not args.use_build:
        command = [
            sys.executable,
            "stick/build_port.py",
            "--output-dir",
            str(build),
            "--arduino-cli",
            args.arduino_cli,
        ]
        if args.config_file:
            command += ["--config-file", str(args.config_file.resolve())]
        subprocess.run(command, check=True)
    manifest = json.loads((build / "build-manifest.json").read_text())
    if manifest["bench_control"]:
        raise RuntimeError("E2E checks require production firmware")
    for name, expected in manifest["source_sha256"].items():
        if digest(ROOT / name) != expected:
            raise RuntimeError(f"Firmware needs rebuilding after changes to {name}")
    for name, expected in manifest["outputs"].items():
        if digest(build / name) != expected:
            raise RuntimeError(f"Firmware hash differs from its build record: {name}")
    binary, rom = prepare(reuse=args.use_prepared_simulator)
    destination = args.output_dir.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="run-", dir=destination))
    policy = out / "diode-feedback"
    subprocess.run(
        [
            "c++",
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "stick/audits/diode_feedback.cpp",
            "-o",
            str(policy),
        ],
        check=True,
    )
    subprocess.run([str(policy)], check=True)
    firmware = out / "firmware.merged.bin"
    shutil.copyfile(build / "PwStick.ino.merged.bin", firmware)
    if digest(firmware) != manifest["outputs"]["PwStick.ino.merged.bin"]:
        raise RuntimeError("Firmware changed while retaining the run's immutable image")
    (out / "build-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    evidence = {
        "application_sha256": manifest["outputs"]["PwStick.ino.bin"],
        "merged_sha256": digest(firmware),
        "firmware_sources_sha256": manifest["source_sha256"],
        "simulator_revision": REVISION,
        "simulator_patch_sha256": digest(ROOT / "stick/simulation/soc.patch"),
        "simulator_binary_sha256": digest(binary),
        "simulator_rom_sha256": digest(rom),
        "scenarios": {},
        "complete": False,
    }
    evidence["verification_sources_sha256"] = {
        name: digest(ROOT / name)
        for name in (
            "stick/check.py",
            "stick/simulation/bootstrap.py",
            "stick/simulation/board.rs",
            "stick/simulation/main.rs",
            "stick/simulation/Cargo.toml",
            "stick/simulation/Cargo.lock",
            "stick/simulation/settings.actions",
            "stick/simulation/connect.actions",
            "stick/simulation/diode.actions",
            "stick/simulation/diode-peer.actions",
            "stick/simulation/diode-ready.actions",
            "stick/simulation/diode-presence.actions",
            "stick/simulation/diode-timeout.actions",
            "stick/simulation/margin-power.actions",
            "stick/simulation/margin-sleep.actions",
            "stick/simulation/connection-details.actions",
            "stick/simulation/diode-correction.actions",
            "stick/simulation/controls.actions",
            "stick/simulation/layouts.actions",
            "stick/simulation/wake.actions",
            "stick/simulation/recovery.actions",
            "stick/simulation/peer.actions",
            "stick/simulation/peer-storage.actions",
            "stick/simulation/peer-transfer.actions",
            "stick/simulation/peer-transfer-compressed.actions",
            "stick/simulation/peer-play.actions",
            "stick/simulation/peer.rs",
            "stick/simulation/fixture.py",
            "stick/audits/diode_feedback.cpp",
        )
    }
    scenarios = shard_scenarios(*args.shard) if args.shard else (args.scenario or SCENARIOS)
    fixture = None
    bare_scenarios = (
        "settings",
        "connect",
        "diode",
        "diode-presence",
        "diode-timeout",
        "diode-stall",
        "diode-ready",
    )
    if any(name not in bare_scenarios for name in scenarios):
        fixture = storage(build / "PwStick.ino.partitions.bin", out)
        evidence["fixture_sha256"] = digest(out / "fixture.eeprom")
    (out / "results.json").write_text(json.dumps(evidence, indent=2) + "\n")
    for scenario in scenarios:
        folder = out / scenario
        folder.mkdir(exist_ok=True)
        scenario_fixture = fixture
        if scenario.startswith("recovery-") or scenario in ("peer-play", "peer-return"):
            scenario_fixture = storage(
                build / "PwStick.ino.partitions.bin",
                folder,
                "peer-play" if scenario == "peer-return" else scenario,
            )
        action = "recovery" if scenario.startswith("recovery-") else scenario
        if scenario == "diode-stall":
            action = "diode-timeout"
        if scenario in PUT_SCENARIOS or scenario == "peer-return":
            action = (
                "peer-transfer-compressed"
                if scenario == "peer-transfer-compressed"
                else "peer-transfer"
            )
        if scenario == "peer-malformed":
            action = "peer-storage"
        if scenario == "peer-keepalive-only":
            action = "peer"
        script = ROOT / f"stick/simulation/{action}.actions"
        if scenario in OPTICAL_SCENARIOS:
            script = folder / f"{scenario}.actions"
            actions = (ROOT / f"stick/simulation/{action}.actions").read_text()
            if scenario in PUT_SCENARIOS:
                source = folder / "course.bin"
                source.write_bytes(course())
            else:
                source = (
                    out / "fixture.eeprom"
                    if scenario == "peer-malformed"
                    else folder / "fixture.eeprom"
                )
            if scenario in ("peer-noise", "peer-prefix-byte"):
                actions = actions.replace(
                    "peer echo on",
                    "peer echo on\npeer noise "
                    + ("byte" if scenario == "peer-prefix-byte" else "on"),
                )
            if scenario == "peer-initial":
                initial = bytearray((out / "fixture.eeprom").read_bytes())
                for address in range(0x280, 0x8CB0, 128):
                    initial[address] ^= 1
                uploaded = folder / "initial-image.eeprom"
                uploaded.write_bytes(initial)
                actions = actions.replace(
                    "peer put PUBLIC_FIXTURE",
                    "peer put-initial PUBLIC_FIXTURE " + str(uploaded),
                )
            if scenario == "peer-return":
                actions = actions.replace("peer put PUBLIC_FIXTURE", "peer return")
            if scenario == "peer-slow":
                actions = actions.replace("115760", "114817")
            # Circuit inputs are physical quantities. These scenarios send the
            # same public full-transfer fixture through GPIO5; no sample bins
            # or packet bytes are supplied to the production receiver.
            circuits = {
                "peer-rc-low": "22000 12 1.6 60 0.05",
                "peer-rc-high": "65000 12 1.6 60 0.05",
                "peer-rc-gap": "45000 11 1.5 80 0.05",
            }
            if scenario in circuits:
                actions = actions.replace(
                    "peer echo on", "peer echo on\npeer circuit " + circuits[scenario]
                )
            if scenario == "peer-rc-gap":
                actions = actions.replace("115760", "114817")
                actions = actions.replace(
                    "peer echo on", "peer echo on\npeer waveform 347 339 4166"
                )
            if scenario == "peer-immediate":
                actions = actions.replace("115760 5000", "115760 1")
            if scenario == "peer-deadline":
                # FA crosses the native receive deadline; F8 needs no flash
                # write and can still meet the peer's independent 100 ms limit.
                actions = actions.replace("peer echo on", "peer echo on\npeer delay-frame fa 97200")
            if scenario == "peer-keepalive":
                actions = actions.replace("peer echo on", "peer echo on\npeer keepalive prefix")
            if scenario == "peer-keepalive-only":
                actions = actions.replace("peer start", "peer start\npeer keepalive only")
                actions = actions.replace(
                    "run 4000\npeer report", "run-until-peer 4000\nrun 150\npeer report"
                )
                actions = actions.replace("peer report peer.txt", "peer report peer.txt\npeer stop")
            if scenario == "peer-malformed":
                template = (ROOT / "stick/simulation/peer.actions").read_text()
                first = template.index("peer start")
                last = template.index("peer report")
                transfer = template[first:last]
                actions = "run 4000\n"
                for mode in range(5):
                    actions += transfer.replace(
                        "peer start", f"peer malformed PUBLIC_FIXTURE {mode}"
                    )
                    actions += f"peer report malformed-{mode}.txt\npeer stop\nreset\nrun 4000\n"
            actions = re.sub(r"(?m)^peer report (\S+)$", r"peer report \1\nsram \1.sram", actions)
            script.write_text(actions.replace("PUBLIC_FIXTURE", str(source)))
        if scenario == "battery":
            script = folder / "battery.actions"
            actions = (ROOT / "stick/simulation/wake.actions").read_text()
            actions = actions.replace(
                "screen asleep.png", "run-until-timer-wake 1000\nscreen asleep.png"
            )
            script.write_text("vbus 0\n" + actions)
        # Exercise the unsaved three-key default, then explicitly select the
        # two-key layout used by the established navigation/rollover scripts.
        actions = script.read_text()
        if scenario == "diode-stall":
            actions = actions.replace("touch timeout 1", "touch hang 1")
        script = folder / "configured.actions"
        script.write_text(
            "run 8000\npress L\npress R\nrun 1200\nrelease L\nrelease R\n"
            "run 200\nscreen defaults.png\n"
            "press R\nrun 70\nrelease R\nrun 300\n"
            "press R\nrun 70\nrelease R\nrun 300\n"
            "press L\nrun 70\nrelease L\nrun 300\n" + actions
        )
        if scenario in ("diode-timeout", "diode-stall"):
            script.write_text(actions)
        with (folder / "run.log").open("w") as log:
            subprocess.run(
                [
                    str(binary),
                    str(rom),
                    str(firmware),
                    str(script),
                    str(folder),
                ]
                + (
                    [str(scenario_fixture[0]), str(scenario_fixture[1])]
                    if scenario not in bare_scenarios
                    else []
                ),
                stdout=log,
                stderr=log,
                check=True,
                timeout=900,
                env={
                    **os.environ,
                    **(
                        {"STICK_E2E_OPCODE_TIMING": "1", "STICK_E2E_FLASH_TIMING": "300,20000"}
                        if scenario in OPTICAL_SCENARIOS
                        else {}
                    ),
                },
            )
        if scenario == "settings":
            expect_reboot(folder)
            console = (folder / "console.txt").read_text()
            assert "PW_STICK_DIAGNOSTICS version=1 " in console
            assert "PW_STICK_LINK none" in console
            if screen_region(folder / "welcome.png", OVERLAY_HEADER) == screen_region(
                folder / "settings.png", OVERLAY_HEADER
            ):
                raise AssertionError("Opening settings did not change the screen")
            if screen_region(folder / "defaults.png", (10, 42, 165, 52)) == screen_region(
                folder / "settings.png", (10, 42, 165, 52)
            ):
                raise AssertionError("Selecting the two-key layout did not change the setting")
            expect_appearance(folder / "settings.png", light=False)
            for name in ("changed.png", "retained.png"):
                expect_appearance(folder / name, light=True)
                if screen_region(folder / "settings.png", OVERLAY_HEADER) != screen_region(
                    folder / name, OVERLAY_HEADER, rotation=-90, invert=True
                ):
                    raise AssertionError("Settings did not rotate with the display")
            if screen_region(folder / "changed.png", (20, 24, 240, 120), rotation=-90) != (
                screen_region(folder / "retained.png", (20, 24, 240, 120), rotation=-90)
            ):
                raise AssertionError("Changed settings did not survive reset")
            evidence["scenarios"][scenario] = (
                "Unsaved controls use L/M/R and left-side-down; buttons change rotation and appearance, and reset retains both."
            )
        elif scenario == "connect":
            expect_diode(folder / "inconclusive-attempt.png", None)
            uncertain = []
            for path in sorted(folder.glob("no-peer-[0-9][0-9].png")):
                try:
                    expect_diode(path, "unknown")
                    uncertain.append(path)
                except AssertionError:
                    pass
            assert uncertain, "Inconclusive no-reply result never showed uncertain feedback"
            shutil.copyfile(uncertain[0], folder / "no-peer.png")
            expect_diode(folder / "no-peer-cleared.png", None)
            # The native home face is stable; the blinking arrow is outside
            # this crop. Margin clearance alone does not establish dismissal.
            home_body = (88, 35, 152, 75)
            assert screen_region(folder / "no-peer-cleared.png", home_body) == screen_region(
                folder / "retry-home.png", home_body
            ), "No-reply result did not return to the visible native home"
            expect_overlay(folder, folder / "settings-after.png")
            expect_overlay(folder, folder / "settings-retry.png")
            if (folder / "first.optical").read_text().strip():
                raise AssertionError("Held-low input must fail preflight before transmitting")
            retry = optical_frames(folder / "retry.optical")
            if bytes([0xFC]) not in retry:
                raise AssertionError("Retry after clearing the input must advertise")
            evidence["scenarios"][scenario] = {
                "result": "Held-low input is rejected before transmission; an inconclusive diode check permits retry without a hardware warning on no reply; Settings remains usable.",
                "logical_frames": [frame.hex() for frame in retry],
            }
        elif scenario == "margin-power":
            for name in (
                "healthy",
                "unplugged-expired",
                "plugged-expired",
                "failed-read",
                "recovered-read",
                "recovered",
                "unavailable",
                "low-usb-stable",
                "bounce",
            ):
                expect_battery(folder / f"{name}.png", None)
            expect_battery(folder / "unplugged.png", 4)
            expect_battery(folder / "plugged.png", 4, True)
            for name in (
                "low",
                "held-low",
                "low-again",
                "low-unplugged",
                "low-read-failed",
                "low-supply-unknown",
                "low-supply-restored",
            ):
                expect_battery(folder / f"{name}.png", 0)
            expect_battery(folder / "low-plugged.png", 0, True)
            for path in folder.glob("*.png"):
                if path.name != "defaults.png":
                    expect_diode(path, None)
            evidence["scenarios"][scenario] = (
                "Healthy play is quiet; USB changes appear once then expire; failed reads produce no false event; low warning is steady with voltage hysteresis and suppressed on stable USB."
            )
        elif scenario == "diode-correction":
            for name in ("blocked", "first-check", "unstable"):
                expect_diode(folder / f"{name}.png", "blocked")
            frames = sorted(folder.glob("settling-*.png"))
            shown = []
            for index, path in enumerate(frames):
                tile = Image.open(path).rotate(90, expand=True).convert("L")
                bits = bytes(
                    tile.getpixel((4 + x, 4 + y)) >= 128 for y in range(16) for x in range(16)
                )
                if (
                    hashlib.sha256(bits).hexdigest()
                    == "59df9fbf1e7334530d2c4dfdb0f6abd4aea4de8f585b714e208583cacc793cbd"
                ):
                    shown.append(index)
            assert 9 <= len(shown) <= 11, (
                f"Unexpected frame count for two-second correction: {shown}"
            )
            assert shown == list(range(shown[0], shown[-1] + 1)), "Correction indicator flickered"
            assert shown[0] <= 22, "Two stable rechecks did not recognize correction promptly"
            for path in frames[: shown[0]]:
                expect_diode(path, "blocked")
            for path in frames[shown[-1] + 1 :]:
                expect_diode(path, None)
            bounded = diode_report(folder / "corrected.txt")
            idle = diode_report(folder / "idle.txt")
            assert bounded["touch_scans"] == idle["touch_scans"], "Resolved warning kept probing"
            evidence["scenarios"][scenario] = (
                "A failed registered attempt retains its warning; one inconsistent safe check does not change it; two stable checks confirm correction for two seconds, then hide and stop probing."
            )
        elif scenario == "connection-details":
            expect_diode(folder / "failed.png", "receiving")
            expect_diode(folder / "returned.png", "receiving")
            expect_diode(folder / "left.png", None)
            assert (folder / "details.png").read_bytes() == (
                folder / "details-retained.png"
            ).read_bytes(), "Opening Settings lost or changed the completed attempt"
            log = (folder / "console.txt").read_text()
            assert "preflight=1" in log and "packets=1" in log, log
            assert "result=0 " not in log, "Interrupted exchange reported success"
            evidence["scenarios"][scenario] = (
                "A dropped peer after one packet leaves a steady wave on failure; contradictory preflight evidence and the final result survive Settings; leaving clears the icon."
            )
        elif scenario == "diode":
            for name in ("blocked", "bright", "resistive", "reversed", "rotated"):
                report = (folder / f"{name}.txt").read_text()
                if "high_drives=0" not in report or "reboots=0" not in report:
                    raise AssertionError(f"Blocked diode entered precharge or rebooted: {report}")
                if "output_enabled=false" not in report:
                    raise AssertionError(f"Diode output was not released: {report}")
            if (folder / "blocked.optical").read_text().strip():
                raise AssertionError("A blocked diode transmitted before preflight completed")
            icons = {
                "reversed": "199b72c6f55f2b4aed8c7a6e5e8cb2e2eefb3b85f80ce4bd9cb03c0a72666157",
                "connected": "59df9fbf1e7334530d2c4dfdb0f6abd4aea4de8f585b714e208583cacc793cbd",
            }
            for name, state, rotated in (
                ("reversed", "reversed", False),
                ("rotated", "reversed", True),
            ):
                image = (
                    Image.open(folder / f"{name}.png")
                    .rotate(-90 if rotated else 90, expand=True)
                    .convert("L")
                )
                x = 220 if rotated else 4
                y = 115 if rotated else 4
                bits = bytes(
                    (
                        image.getpixel((x + col, y + row)) < 128
                        if rotated
                        else image.getpixel((x + col, y + row)) >= 128
                    )
                    for row in range(16)
                    for col in range(16)
                )
                if hashlib.sha256(bits).hexdigest() != icons[state]:
                    raise AssertionError(f"{name}: diode icon differs from expected artwork")
            for name, rotated in (
                ("idle", False),
                ("rotated-left", True),
                ("rotated-persist", True),
            ):
                image = (
                    Image.open(folder / f"{name}.png")
                    .rotate(-90 if rotated else 90, expand=True)
                    .convert("L")
                )
                x = 220 if rotated else 4
                y = 115 if rotated else 4
                if any(
                    (
                        image.getpixel((x + col, y + row)) < 128
                        if rotated
                        else image.getpixel((x + col, y + row)) >= 128
                    )
                    for row in range(16)
                    for col in range(16)
                ):
                    raise AssertionError(f"{name}: diode icon did not clear")
            expect_diode(folder / "corrected.png", None)
            expect_diode(folder / "expired.png", None)
            for name in ("unknown", "bright", "resistive"):
                expect_diode(folder / f"{name}.png", "blocked")
            evidence["scenarios"][scenario] = (
                "Ambiguous and illuminated-reversed inputs never drive GPIO5 high or reboot; "
                "icons occupy the diode margin in both rotations and themes, "
                "blocked inputs show a warning, faults persist while relevant, and leaving clears feedback."
            )
        elif scenario == "diode-peer":
            peer = (folder / "peer.txt").read_text()
            if "complete=true error=None" not in peer:
                raise AssertionError(f"Optical peer did not complete: {peer}")
            expect_diode(folder / "connected.png", None)
            expect_diode(folder / "retained.png", None)
            expect_diode(folder / "expired.png", None)
            evidence["scenarios"][scenario] = (
                "Successful exchange clears feedback immediately without a trailing confirmation."
            )
        elif scenario == "diode-ready":
            observed = False
            for index in range(8):
                try:
                    expect_diode(folder / f"ready-{index}.png", "connected")
                    observed = True
                except AssertionError:
                    pass
            assert observed, "Connected diode was never shown during connection"
            expect_diode(folder / "finished.png", None)
            diode_report(folder / "finished.txt")
            evidence["scenarios"][scenario] = (
                "Diode presence is shown during the attempt and hidden after an otherwise uninformative no-reply result."
            )
        elif scenario == "diode-presence":
            for name in ("missing", "missing-connect", "missing-uncertain"):
                expect_diode(folder / f"{name}.png", "missing")
            expect_diode(folder / "inserted.png", None)
            expect_diode(folder / "missing-persist.png", None)
            expect_diode(folder / "inserted-expired.png", None)
            for name in ("missing-left",):
                expect_diode(folder / f"{name}.png", None)
            bounded = diode_report(folder / "missing-bounded.txt")
            idle = diode_report(folder / "missing-idle.txt")
            assert int(idle["touch_scans"]) >= int(bounded["touch_scans"]), (
                "Probe counter moved backwards"
            )
            blocked = diode_report(folder / "missing-connect.txt", require_released=False)
            assert int(blocked["high_drives"]) > 0, "Advisory absence blocked safe reception"
            assert bytes([0xFC]) in optical_frames(folder / "missing.optical"), (
                "An uncertain absence estimate prevented listening"
            )
            diode_report(folder / "restored.txt")
            evidence["scenarios"][scenario] = (
                "Empty capacitance is advisory and permits safe listening; "
                "the warning follows the result, recognizes stable correction, and clears after confirmation."
            )
        elif scenario in ("diode-timeout", "diode-stall"):
            expect_diode(folder / "unknown.png", None)
            expect_diode(folder / "unknown-expired.png", None)
            bounded = diode_report(folder / "timeout-bounded.txt")
            idle = diode_report(folder / "timeout-idle.txt")
            assert int(idle["touch_scans"]) >= int(bounded["touch_scans"]), (
                "Probe counter moved backwards"
            )
            evidence["scenarios"][scenario] = (
                "Stalled scans remain inconclusive, return within the deadline and release the controller."
            )
        elif scenario == "controls":
            headers = {
                name: screen_region(folder / f"{name}.png", NATIVE_HEADER)
                for name in ("settings-menu", "inventory", "trainer")
            }
            if len(set(headers.values())) != 3:
                raise AssertionError("Repeated taps did not reach three distinct menu states")
            for name, previous in (
                ("inventory-return", "inventory"),
                ("settings-return", "settings-menu"),
                ("rollover", "settings-menu"),
                ("native-settings", "settings-menu"),
            ):
                if screen_region(folder / f"{name}.png", NATIVE_HEADER) != headers[previous]:
                    raise AssertionError(f"{name}: navigation did not return to its prior state")
            for name in ("overlay-held", "overlay-released", "overlay-fresh"):
                expect_overlay(folder, folder / f"{name}.png")
            region = (10, 42, 165, 52)
            held = screen_region(folder / "overlay-held.png", region)
            if held != screen_region(folder / "overlay-released.png", region):
                raise AssertionError("Held R leaked through overlay capture before release")
            if held == screen_region(folder / "overlay-fresh.png", region):
                raise AssertionError("Fresh R after release did not change the layout")
            evidence["scenarios"][scenario] = {
                "result": "Short repeated and alternating taps navigate; rollover does not become Center; overlay captures held inputs until release.",
                "tones": expect_tones(folder / "navigation.pcm", 5),
            }
        elif scenario == "layouts":
            labels = []
            for index in range(4):
                before, after = (
                    folder / f"{prefix}{index}.png" for prefix in ("layout", "retained")
                )
                expect_overlay(folder, before)
                expect_overlay(folder, after)
                label = screen_region(before, (10, 42, 165, 52))
                if label != screen_region(after, (10, 42, 165, 52)):
                    raise AssertionError(f"Layout {index} did not survive reset")
                labels.append(label)
            if len(set(labels)) != 4 or labels[0] != screen_region(
                folder / "wrapped0.png", (10, 42, 165, 52)
            ):
                raise AssertionError("Layout selection did not cycle through four distinct choices")
            if labels[2] != screen_region(folder / "defaults.png", (10, 42, 165, 52)):
                raise AssertionError("Unsaved controls did not select the three-key L/R layout")
            expect_overlay(folder, folder / "two-key-reopened.png")
            if labels[0] != screen_region(folder / "two-key-reopened.png", (10, 42, 165, 52)):
                raise AssertionError("Dedicated L did not reopen the retained two-key layout")
            for mapping, index in (("LR", 2), ("RL", 3)):
                direction = folder / f"direction-{mapping}.png"
                pending = folder / f"pending-{mapping}.png"
                if screen_region(direction, NATIVE_HEADER) != screen_region(
                    folder / "two-key-native.png", NATIVE_HEADER
                ):
                    raise AssertionError("Gameplay input did not return to the native menu")
                if screen_region(direction, (24, 24, 184, 72)) == screen_region(
                    pending, (24, 24, 184, 72)
                ):
                    raise AssertionError("Joining the Settings hold withheld gameplay Right")
                if screen_region(pending, OVERLAY_HEADER) == screen_region(
                    folder / "defaults.png", OVERLAY_HEADER
                ):
                    raise AssertionError("Settings opened before its one-second hold")
                expect_cursor(folder / f"two-choice-{mapping}.png", 48, 20)
                for prefix in ("held", "partial"):
                    path = folder / f"{prefix}-{mapping}.png"
                    expect_overlay(folder, path)
                    if labels[index] != screen_region(path, (10, 42, 165, 52)):
                        raise AssertionError("Opening hold operated the Settings overlay")
            for name, x, y in (
                ("volume", 64, 44),
                ("volume-changed", 0, 44),
                ("contrast", 40, 32),
                ("contrast-changed", 48, 32),
            ):
                expect_cursor(folder / f"{name}.png", x, y)
            expect_saved_settings(folder, fixture[0], fixture[1].stat().st_size)
            evidence["scenarios"][scenario] = (
                "All four layouts persist; L+R delivers gameplay before opening Settings, consumes held buttons through release, and preserves the native two-choice selector and mirrored saves."
            )
        elif scenario == "margin-sleep":
            for name in ("asleep", "held", "released"):
                expect_battery(folder / f"{name}.png", None)
            assert "sleeping=true" in (folder / "usb-asleep.txt").read_text()
            evidence["scenarios"][scenario] = (
                "Changing USB while asleep does not power the display or replay a battery indication on wake."
            )
        elif scenario in ("wake", "battery"):
            for name in ("asleep", "ignored", "cancelled"):
                state = (folder / f"{name}.txt").read_text()
                if "sleeping=true" not in state or "peripheral_supply=false" not in state:
                    raise AssertionError(
                        f"{name}: expected suspended panel and released shared supply; {state}"
                    )
            if "sleeping=false" not in (folder / "held.txt").read_text():
                raise AssertionError("Qualified M hold did not wake the panel")
            if scenario == "battery":
                asleep = dict(
                    line.split("=", 1)
                    for line in (folder / "asleep.txt").read_text().splitlines()[1:]
                )
                held = dict(
                    line.split("=", 1)
                    for line in (folder / "held.txt").read_text().splitlines()[1:]
                )
                if int(asleep["rtc_wakeup"]) & 8 == 0 or int(asleep["ext1_mask"]) != 1 << 11:
                    raise AssertionError(
                        "Battery idle must reach RTC timer sleep with M-only EXT1 wake"
                    )
                if (
                    int(held["rtc_wakeup"]) & 2 == 0
                    or held["main_rtc_mux"] != "0"
                    or held["side_rtc_mux"] != "0"
                ):
                    raise AssertionError(
                        "M must wake via EXT1 and both front buttons must return to digital GPIO"
                    )
            if screen_region(folder / "held.png", (24, 3, 216, 131)) != screen_region(
                folder / "released.png", (24, 3, 216, 131)
            ):
                raise AssertionError("Wake hold/release leaked a native navigation action")
            if screen_region(folder / "first-action.png", NATIVE_HEADER) == screen_region(
                folder / "repeated-action.png", NATIVE_HEADER
            ):
                raise AssertionError("The first and repeated wake actions reached the same menu")
            expect_overlay(folder, folder / "settings.png")
            evidence["scenarios"][scenario] = {
                "result": "Native timeout sleeps; R/L do not wake; short M cancels; qualified M wakes and is consumed through release.",
                "vbus_mv": 0 if scenario == "battery" else 5000,
                "cold": expect_tones(folder / "cold.pcm", 1),
                "warm": expect_tones(folder / "warm.pcm", 1),
            }
        elif scenario.startswith("recovery-"):
            expect_reboot(folder)
            if screen_region(folder / "before-reset.png", NATIVE_HEADER) != screen_region(
                folder / "after-reset.png", NATIVE_HEADER
            ):
                raise AssertionError("Recovery did not return to the previous menu")
            payload = saved_eeprom(folder, scenario_fixture[0], scenario_fixture[1].stat().st_size)
            watts = 100 if scenario == "recovery-journal" else 200
            for address in (0x156, 0x256):
                record = payload[address : address + 25]
                if (
                    record[24] != (1 + sum(record[:24])) & 255
                    or int.from_bytes(record[14:16], "big") != watts
                ):
                    raise AssertionError(
                        f"{scenario}: both recovered save mirrors must retain {watts} Watts"
                    )
            evidence["scenarios"][scenario] = {
                "watts": watts,
                "fixture_files_sha256": {
                    path.name: digest(path) for path in (folder / "fixture.store",)
                },
            }
        elif scenario in EXCHANGE_SCENARIOS:
            expect_reboot(folder)
            report = (folder / "peer.txt").read_text()
            if "complete=true error=None" not in report:
                raise AssertionError(f"{scenario} failed:\n{report[:1500]}")
            if "flash_erases_during_reply_window=0" not in report:
                raise AssertionError("Flash erase occurred while a peer reply was pending")
            payload = saved_eeprom(folder, scenario_fixture[0], scenario_fixture[1].stat().st_size)
            if scenario in PUT_SCENARIOS:
                if payload[0x8F00 : 0x8F00 + 10430] != course():
                    raise AssertionError("DS put course differs after native commit/reset")
                identity = (out / "fixture.eeprom").read_bytes()[0x83 : 0x83 + 40]
                if payload[0xCC08:0xCC30] != identity:
                    raise AssertionError("DS trainer record did not survive native commit/reset")
                frames = 416 if scenario == "peer-initial" else 135
                if f"received_frames={frames}" not in report or "watts=0064" not in report:
                    raise AssertionError(
                        "DS put did not complete course/record writes and PHC/distribution reads"
                    )
                if scenario == "peer-initial":
                    original = (out / "fixture.eeprom").read_bytes()
                    expected = (folder / "initial-image.eeprom").read_bytes()
                    assert original[0x280:0x8ED0] != expected[0x280:0x8ED0]
                    assert payload[0x280:0x8ED0] == expected[0x280:0x8ED0], (
                        "Initial image differs after commit"
                    )
            elif scenario == "peer-return":
                for address in (0xED, 0x1ED):
                    assert payload[address + 91] & 3 == 1, (
                        "Return did not clear companion while preserving registration"
                    )
                original = (folder / "fixture.eeprom").read_bytes()
                assert payload[0x83 : 0x83 + 40] == original[0x83 : 0x83 + 40], (
                    "Return changed device identity"
                )
            else:
                identity = b"PUBLIC OPTICAL WALKER".ljust(40, b"\0")
                if payload[0xDC00 + 548 + 8 : 0xDC00 + 548 + 48] != identity:
                    raise AssertionError("Native peer completion did not persist peer history")
                local = (folder / "fixture.eeprom").read_bytes()
                for source, destination, length in ((0x91BE, 0xF400, 384), (0x993E, 0xF580, 320)):
                    expected = bytes(byte ^ 0x5A for byte in local[source : source + length])
                    if payload[destination : destination + length] != expected:
                        raise AssertionError("Peer animation/name did not persist")
            expect_overlay(folder, folder / "settings-after.png")
            evidence["scenarios"][scenario] = {
                "result": "DS put/commit or native Walker peer exchange completes with retail baud, transmitter echo and instruction-cost timing; persisted state survives reset.",
                "timing": "one-instruction quanta with opcode/branch/call/load-use costs; 300 us program and 20 ms sector erase busy periods; no analog/cache timing claim",
            }
            if scenario == "peer-deadline":
                probe = re.search(r"(?m)^(\d+) WALKER fc$", report)
                delayed = re.search(r"(?m)^(\d+) DELAYED fa (\d+)$", report)
                reply = re.search(r"(?m)^(\d+) WALKER f8[0-9a-f]+$", report)
                assert probe and delayed and reply, report
                origin = int(probe[1])
                start, end = (int(value) - origin for value in delayed.groups())
                completed = int(reply[1]) - origin
                assert start == 97_200 * 240, report
                assert end > 97_700 * 240, report
                assert completed < 100_000 * 240, report
                evidence["scenarios"][scenario]["delayed_handshake"] = {
                    "fa_start_us_after_fc": start / 240,
                    "fa_end_us_after_fc": end / 240,
                    "f8_end_us_after_fc": completed / 240,
                }
        elif scenario == "peer-keepalive-only":
            assert "reboots=0" in (folder / "run.log").read_text(), (
                "Ignored traffic rebooted firmware"
            )
            report = (folder / "peer.txt").read_text()
            values = dict(line.split("=", 1) for line in report.splitlines() if "=" in line)
            assert values["received_frames"] == "2", report
            assert values["injected_keepalives"] == "100", report
            # GPIO charge transitions expose the real sampling lifetime. This
            # assertion cannot be satisfied merely by changing a status flag.
            listening_cycles = int(values["last_charge_cycle"]) - int(values["last_reply_cycle"])
            assert 90 * 240_000 <= listening_cycles <= 120 * 240_000, report
            evidence["scenarios"][scenario] = {
                "result": "Ignored valid packets do not grant a storage turn or extend the fixed receive deadline.",
                "listening_us_after_last_reply": listening_cycles / 240,
                "injected_keepalives": 100,
            }
        elif scenario == "peer-malformed":
            for mode in range(5):
                report = (folder / f"malformed-{mode}.txt").read_text()
                assert "complete=true" in report and "page_verified=true" in report, report
            log = (folder / "console.txt").read_text()
            assert log.count("malformed=1") == 5, "Each malformed packet must be diagnosed"
            evidence["scenarios"][scenario] = (
                "Malformed headers/bodies and H8 RAM writes are rejected; a subsequent valid read completes and the target page remains unchanged."
            )
        elif scenario == "peer":
            report = (folder / "peer.txt").read_text()
            if (
                "complete=true" not in report
                or "error=none" not in report
                or "watts=0064" not in report
            ):
                raise AssertionError(f"Functional DS exchange failed:\n{report}")
            frames = optical_frames(folder / "peer.optical")
            replies = [
                frame for frame in frames if len(frame) == 10 and frame[:2] == bytes((0x0E, 2))
            ]
            if len(replies) != 1 or replies[0][8:] != bytes((0, 100)):
                raise AssertionError(
                    "Status/read exchange must emit 100 Watts as 00 64 on the optical wire"
                )
            expect_overlay(folder, folder / "settings-after.png")
            evidence["scenarios"][scenario] = {
                "result": "Ideal optical DS status/read session emits big-endian Watts and returns control to Settings.",
                "logical_frames": [frame.hex() for frame in frames],
                "peer": report,
            }
        elif scenario == "peer-storage":
            expect_reboot(folder)
            for name in ("written", "restored"):
                report = (folder / f"{name}.txt").read_text()
                if any(
                    field not in report
                    for field in ("complete=true", "error=none", "page_verified=true", "watts=0064")
                ):
                    raise AssertionError(f"Optical page write/reboot/read failed:\n{report}")
            frames = optical_frames(folder / "peer.optical")
            replies = [
                frame[8:] for frame in frames if len(frame) == 136 and frame[:2] == bytes((0x0E, 2))
            ]
            expected = bytes(index ^ 0x5A for index in range(120)) + bytes.fromhex(
                "fa013aa913579bdf"
            )
            if replies != [expected, expected]:
                raise AssertionError("Optical page payload must survive a reset and second session")
            expect_overlay(folder, folder / "settings-after.png")
            payload = saved_eeprom(folder, fixture[0], fixture[1].stat().st_size)
            if payload[0xFF80:] != expected:
                raise AssertionError(
                    "Acknowledged optical page is absent from the durable EEPROM checkpoint"
                )
            evidence["scenarios"][scenario] = (
                "An optical EEPROM page write reads back immediately and after reboot; its durable checkpoint contains the same public pattern."
            )
        evidence.setdefault("scenario_actions_sha256", {})[scenario] = digest(script)
        (out / "results.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(f"{scenario}: PASS", flush=True)
    render_margin_atlas(out)
    evidence["complete"] = True
    (out / "results.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"Firmware E2E checks passed. Screens and signal recordings: {out}")


if __name__ == "__main__":
    main()
