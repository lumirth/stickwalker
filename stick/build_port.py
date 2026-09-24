#!/usr/bin/env python3
"""Build the current pw source and Stick drivers with the local M5 toolchain."""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BENCH = ROOT.parent / "bench"
CLI = BENCH / ".tools" / "arduino-cli"
CONFIG = BENCH / ".tools" / "arduino-cli.yaml"
SKETCH = ROOT / "stick" / ".build" / "PwStick"
OUTPUT = ROOT / "stick" / ".build" / "output"
EXCLUDE = {
    "src/application/pw_accel_bma150.c",  # BMI270-backed BMA150 register seam
    "src/application/pw_factory_test.c",  # H8-only fixture boot handshake
    "src/application/pw_ssu_init.c",  # H8 shared SPI bus setup
    "src/application/pw_eeprom_m95512_bus.c",  # Stick storage backend
    "src/startup/h8_dbsct.c",  # Renesas section table
    "src/startup/h8_intprg.c",  # Renesas vector table
}


def stage() -> None:
    if SKETCH.exists():
        shutil.rmtree(SKETCH)
    (SKETCH / "src").mkdir(parents=True)
    (SKETCH / "PwStick.ino").write_text(
        "#include <Arduino.h>\n"
        'extern "C" void StickPortSetup(void);\n'
        'extern "C" void StickPortLoop(void);\n'
        "void setup() { StickPortSetup(); }\n"
        "void loop() { StickPortLoop(); }\n"
    )
    for source in ROOT.glob("src/**/*.c"):
        rel = source.relative_to(ROOT)
        if str(rel) in EXCLUDE:
            continue
        destination = SKETCH / "src" / rel
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    for source in (ROOT / "stick").glob("*.cpp"):
        destination = SKETCH / "src" / "stick" / source.name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    for source in (ROOT / "stick").glob("*.c"):
        destination = SKETCH / "src" / "stick" / source.name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def build(verbose: bool, bench_control: bool) -> None:
    if not CLI.is_file() or not CONFIG.is_file():
        raise SystemExit("The existing bench M5 Arduino toolchain is unavailable")
    stage()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    includes = [ROOT / "include", ROOT / "stick", ROOT,
                ROOT / "stick" / "compat", ROOT / "build" / "6.02.02"]
    flags = "-DPW_STICK_S3 "
    if bench_control:
        flags += "-DPW_STICK_BENCH_CONTROL "
    flags += " ".join(f"-I{path}" for path in includes)
    command = [
        str(CLI), "--config-file", str(CONFIG), "compile",
        "--fqbn", "m5stack:esp32:m5stack_sticks3",
        "--output-dir", str(OUTPUT),
        "--build-property", f"compiler.c.extra_flags={flags}",
        "--build-property", f"compiler.cpp.extra_flags={flags}",
        "--warnings", "all",
    ]
    if verbose:
        command.append("--verbose")
    command.append(str(SKETCH))
    subprocess.run(command, check=True, cwd=ROOT)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--bench-control", action="store_true")
    args = parser.parse_args()
    build(args.verbose, args.bench_control)
