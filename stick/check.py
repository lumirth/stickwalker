#!/usr/bin/env python3
"""Build and exercise production firmware on a simulated Stick S3."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "stick/simulation"))
from bootstrap import REVISION, prepare  # noqa: E402


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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "stick/.build/e2e-firmware")
    parser.add_argument(
        "--use-build", action="store_true", help="use a matching retained production build"
    )
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--config-file", type=Path)
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
    binary, rom = prepare()
    destination = args.output_dir.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="run-", dir=destination))
    evidence = {
        "application_sha256": digest(build / "PwStick.ino.bin"),
        "merged_sha256": digest(build / "PwStick.ino.merged.bin"),
        "simulator_revision": REVISION,
        "simulator_patch_sha256": digest(ROOT / "stick/simulation/soc.patch"),
        "scenarios": {},
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
        )
    }
    for scenario in ("settings", "connect"):
        folder = out / scenario
        folder.mkdir(exist_ok=True)
        with (folder / "run.log").open("w") as log:
            subprocess.run(
                [
                    str(binary),
                    str(rom),
                    str(build / "PwStick.ino.merged.bin"),
                    str(ROOT / f"stick/simulation/{scenario}.actions"),
                    str(folder),
                ],
                stdout=log,
                stderr=log,
                check=True,
                timeout=180,
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
        else:
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
        print(f"{scenario}: PASS", flush=True)
    (out / "results.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"Firmware E2E checks passed. Screens and signal recordings: {out}")


if __name__ == "__main__":
    main()
