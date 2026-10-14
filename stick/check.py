#!/usr/bin/env python3
"""Build and exercise production firmware on a simulated Stick S3."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import zlib
from array import array
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "stick/simulation"))
from bootstrap import REVISION, prepare  # noqa: E402
from fixture import filesystem  # noqa: E402

SCENARIOS = (
    "settings",
    "connect",
    "controls",
    "layouts",
    "wake",
    "battery",
    "recovery-stale",
    "recovery-legacy",
    "recovery-truncated",
    "peer",
    "peer-storage",
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_screen(path: Path, rotation: int) -> str:
    image = Image.open(path)
    texts = []
    for scale in (6, 8):
        readable = image.rotate(rotation, expand=True)
        readable = readable.resize((240 * scale, 135 * scale), Image.Resampling.NEAREST)
        target = path.with_name(path.stem + f"-ocr-{rotation}-{scale}.png")
        readable.save(target)
        result = subprocess.run(
            ["tesseract", str(target), "stdout", "--psm", "6"],
            check=True,
            capture_output=True,
            text=True,
        )
        texts.append(result.stdout)
    text = "\n".join(texts)
    path.with_suffix(".txt").write_text(text)
    return text


def expect_screen(path: Path, *phrases: str, rotation: int = -90) -> None:
    text = read_screen(path, rotation)
    for phrase in phrases:
        if phrase not in text:
            raise AssertionError(f"{path}: expected {phrase!r}; read:\n{text}")


def expect_appearance(path: Path, light: bool) -> None:
    brightness = statistics.median(Image.open(path).convert("L").tobytes())
    if (brightness >= 128) != light:
        raise AssertionError(f"{path}: selected appearance does not match the visible background")


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


def screen_region(path: Path, box: tuple[int, int, int, int]) -> bytes:
    return Image.open(path).rotate(-90, expand=True).crop(box).tobytes()


def expect_native_text(path: Path, x: int, y: int, text: str, font_path: Path) -> None:
    font = Image.open(font_path).convert("L")
    screen = Image.open(path).rotate(-90, expand=True).convert("L")
    for position, character in enumerate(text):
        cell = ord(character) - (ord("0") if character.isdigit() else ord("A") - 10)
        for row in range(8):
            for column in range(4):
                expected = (
                    character != " "
                    and column < 3
                    and font.getpixel((cell * 3 + column, row)) < 128
                )
                actual = (
                    screen.getpixel((24 + (x + position * 4 + column) * 2, 3 + (y + row) * 2))
                    >= 128
                )
                if actual != expected:
                    raise AssertionError(f"{path}: native caption differs from {text!r}")


def expect_cursor(path: Path, x: int, y: int) -> None:
    image = Image.open(path).rotate(-90, expand=True).convert("L")
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


def littlefs_tool(args) -> Path:
    if args.mklittlefs:
        tool = args.mklittlefs.resolve()
    else:
        command = [args.arduino_cli]
        if args.config_file:
            command += ["--config-file", str(args.config_file.resolve())]
        data = Path(
            json.loads(
                subprocess.check_output(
                    command + ["config", "get", "directories.data", "--format", "json"]
                )
            )
        )
        tool = data / "packages/m5stack/tools/mklittlefs/4.0.2-db0513a/mklittlefs"
    version = subprocess.check_output([str(tool), "--version"], text=True)
    if "mklittlefs ver. 4.0.2" not in version:
        raise RuntimeError("The synthetic fixture requires mklittlefs 4.0.2")
    return tool


def saved_eeprom(folder: Path, tool: Path, offset: int, size: int) -> bytes:
    partition = folder / "saved.littlefs"
    partition.write_bytes((folder / "flash.bin").read_bytes()[offset : offset + size])
    extracted = folder / "saved-files"
    extracted.mkdir()
    subprocess.run(
        [
            str(tool),
            "-u",
            str(extracted),
            "-b",
            "4096",
            "-p",
            "256",
            "-s",
            str(size),
            str(partition),
        ],
        check=True,
    )
    checkpoints = []
    for name in ("pw2-a.bin", "pw2-b.bin"):
        path = extracted / name
        if not path.exists():
            continue
        data = path.read_bytes()
        magic, generation, sequence, length, crc, header_crc = struct.unpack_from("<6I", data)
        if (
            magic != 0x50574532
            or length != 65536
            or zlib.crc32(data[:20]) != header_crc
            or zlib.crc32(data[24:]) != crc
        ):
            raise AssertionError(f"{path}: invalid migrated checkpoint")
        checkpoints.append((generation, sequence, data[24:]))
    if not checkpoints:
        raise AssertionError("Registered fixture was not migrated to a durable checkpoint")
    return max(checkpoints)[2]


def expect_saved_settings(folder: Path, tool: Path, offset: int, size: int) -> None:
    payload = saved_eeprom(folder, tool, offset, size)
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
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--config-file", type=Path)
    parser.add_argument("--mklittlefs", type=Path)
    parser.add_argument("--scenario", action="append", choices=SCENARIOS)
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
    font_path = ROOT / next(
        name
        for name in manifest["source_sha256"]
        if name.startswith("assets/") and name.endswith("/font.bmp")
    )
    binary, rom = prepare()
    destination = args.output_dir.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="run-", dir=destination))
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
            "stick/simulation/Cargo.lock",
            "stick/simulation/settings.actions",
            "stick/simulation/connect.actions",
            "stick/simulation/controls.actions",
            "stick/simulation/layouts.actions",
            "stick/simulation/wake.actions",
            "stick/simulation/recovery.actions",
            "stick/simulation/peer.actions",
            "stick/simulation/peer-storage.actions",
            "stick/simulation/peer.rs",
            "stick/simulation/fixture.py",
        )
    }
    scenarios = args.scenario or SCENARIOS
    fixture = None
    if any(name not in ("settings", "connect") for name in scenarios):
        tool = littlefs_tool(args)
        fixture = filesystem(tool, build / "PwStick.ino.partitions.bin", out)
        evidence["fixture_sha256"] = digest(out / "fixture.eeprom")
        evidence["mklittlefs_sha256"] = digest(tool)
    (out / "results.json").write_text(json.dumps(evidence, indent=2) + "\n")
    for scenario in scenarios:
        folder = out / scenario
        folder.mkdir(exist_ok=True)
        scenario_fixture = fixture
        if scenario.startswith("recovery-"):
            scenario_fixture = filesystem(
                tool, build / "PwStick.ino.partitions.bin", folder, scenario
            )
        action = "recovery" if scenario.startswith("recovery-") else scenario
        script = ROOT / f"stick/simulation/{action}.actions"
        if scenario == "battery":
            script = folder / "battery.actions"
            actions = (ROOT / "stick/simulation/wake.actions").read_text()
            actions = actions.replace(
                "screen asleep.png", "run-until-timer-wake 1000\nscreen asleep.png"
            )
            script.write_text("vbus 0\n" + actions)
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
                    if scenario not in ("settings", "connect")
                    else []
                ),
                stdout=log,
                stderr=log,
                check=True,
                timeout=360,
            )
        if scenario == "settings":
            expect_screen(folder / "settings.png", "STICK SETTINGS", "Left side down", "Dark")
            expect_appearance(folder / "settings.png", light=False)
            for name in ("changed.png", "retained.png"):
                expect_screen(
                    folder / name, "STICK SETTINGS", "Right side down", "Light", rotation=90
                )
                expect_appearance(folder / name, light=True)
            evidence["scenarios"][scenario] = (
                "Buttons change rotation and appearance; reset retains both."
            )
        elif scenario == "connect":
            expect_screen(folder / "settings-after.png", "STICK SETTINGS")
            expect_screen(folder / "settings-retry.png", "STICK SETTINGS")
            first = optical_frames(folder / "first.optical")
            retry = optical_frames(folder / "retry.optical")
            if bytes([0xFC]) not in first or bytes([0xFC]) not in retry[len(first) :]:
                raise AssertionError("Connect and retry must emit the Pokéwalker advertisement")
            evidence["scenarios"][scenario] = {
                "result": "Receiver held low and held high: Connect finishes, retries transmit, Settings remains usable.",
                "logical_frames": [frame.hex() for frame in retry],
            }
        elif scenario == "controls":
            for name, label in (
                ("settings-menu", "SETTINGS"),
                ("inventory", "INVENTORY"),
                ("trainer", "TRAINER"),
                ("inventory-return", "INVENTORY"),
                ("settings-return", "SETTINGS"),
                ("rollover", "SETTINGS"),
                ("native-settings", "SETTINGS"),
            ):
                expect_screen(folder / f"{name}.png", label)
            for name in ("overlay-held", "overlay-released", "overlay-fresh"):
                expect_screen(folder / f"{name}.png", "STICK SETTINGS")
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
                expect_screen(before, "STICK SETTINGS")
                expect_screen(after, "STICK SETTINGS")
                label = screen_region(before, (10, 42, 165, 52))
                if label != screen_region(after, (10, 42, 165, 52)):
                    raise AssertionError(f"Layout {index} did not survive reset")
                labels.append(label)
            if len(set(labels)) != 4 or labels[0] != screen_region(
                folder / "wrapped0.png", (10, 42, 165, 52)
            ):
                raise AssertionError("Layout selection did not cycle through four distinct choices")
            expect_screen(folder / "two-key-native.png", "SETTINGS")
            expect_cursor(folder / "two-key-native.png", 0, 20)
            expect_screen(folder / "two-key-reopened.png", "STICK SETTINGS")
            if labels[0] != screen_region(folder / "two-key-reopened.png", (10, 42, 165, 52)):
                raise AssertionError("Dedicated L did not reopen the retained two-key layout")
            for name in ("launcher-L", "launcher-R"):
                expect_native_text(folder / f"{name}.png", 20, 16, "STICK SETTINGS", font_path)
                expect_native_text(folder / f"{name}.png", 16, 32, "CENTER TO OPEN", font_path)
            for name, x, y in (
                ("volume", 64, 44),
                ("volume-changed", 0, 44),
                ("contrast", 40, 32),
                ("contrast-changed", 48, 32),
            ):
                expect_cursor(folder / f"{name}.png", x, y)
            expect_saved_settings(folder, tool, fixture[0], fixture[1].stat().st_size)
            evidence["scenarios"][scenario] = (
                "All four layouts persist; both three-key mappings reach the explicit launcher, preserving native volume/contrast edits and mirrored saves."
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
            expect_screen(folder / "first-action.png", "SETTINGS")
            expect_screen(folder / "repeated-action.png", "INVENTORY")
            expect_screen(folder / "settings.png", "STICK SETTINGS")
            evidence["scenarios"][scenario] = {
                "result": "Native timeout sleeps; R/L do not wake; short M cancels; qualified M wakes and is consumed through release.",
                "vbus_mv": 0 if scenario == "battery" else 5000,
                "cold": expect_tones(folder / "cold.pcm", 1),
                "warm": expect_tones(folder / "warm.pcm", 1),
            }
        elif scenario.startswith("recovery-"):
            for name in ("before-reset", "after-reset"):
                expect_screen(folder / f"{name}.png", "SETTINGS")
            payload = saved_eeprom(
                folder, tool, scenario_fixture[0], scenario_fixture[1].stat().st_size
            )
            watts = 100 if scenario == "recovery-legacy" else 200
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
                    path.name: digest(path) for path in sorted((folder / "fixture-files").iterdir())
                },
            }
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
            expect_screen(folder / "settings-after.png", "STICK SETTINGS")
            evidence["scenarios"][scenario] = {
                "result": "Ideal optical DS status/read session emits big-endian Watts and returns control to Settings.",
                "logical_frames": [frame.hex() for frame in frames],
                "peer": report,
            }
        elif scenario == "peer-storage":
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
            expected = bytes(index ^ 0x5A for index in range(128))
            if replies != [expected, expected]:
                raise AssertionError("Optical page payload must survive a reset and second session")
            expect_screen(folder / "settings-after.png", "STICK SETTINGS")
            payload = saved_eeprom(folder, tool, fixture[0], fixture[1].stat().st_size)
            if payload[0xFF80:] != expected:
                raise AssertionError(
                    "Acknowledged optical page is absent from the durable EEPROM checkpoint"
                )
            evidence["scenarios"][scenario] = (
                "An optical EEPROM page write reads back immediately and after reboot; its durable checkpoint contains the same public pattern."
            )
        (out / "results.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(f"{scenario}: PASS", flush=True)
    evidence["complete"] = True
    (out / "results.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"Firmware E2E checks passed. Screens and signal recordings: {out}")


if __name__ == "__main__":
    main()
