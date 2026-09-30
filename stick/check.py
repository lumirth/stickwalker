#!/usr/bin/env python3
"""Run Stickwalker host regressions without a board, ROM, or Renesas compiler."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
C_TESTS = {
    "bitfield_layout": [],
    "wire_endian": [],
    "bulk_decode": ["src/support/lib_common.c"],
    "scratch": ["src/support/lib_common.c"],
    "eeprom_mirror": [
        "src/application/pw_eeprom_m95512_io.c",
        "src/support/lib_common.c",
    ],
    "diary_storage": ["src/application/pw_diary.c"],
    "peer_gift": ["src/application/pw_friend.c"],
    "rtc_tick": ["src/application/pw_rtc.c"],
}
CPP_TESTS = {
    "controls": ["stick/controls.cpp"],
    "device_settings": [],
    "display_bus": ["stick/display_bus.cpp"],
    "ir_rx_core": ["stick/ir_rx_core.cpp"],
}


def main() -> None:
    os.chdir(ROOT)
    subprocess.run(
        [sys.executable, "-m", "unittest", "discover", "-s", "tests"], check=True
    )
    subprocess.run(
        [
            sys.executable,
            "-m",
            "unittest",
            "discover",
            "-s",
            "stick/tests",
            "-p",
            "test_*.py",
        ],
        check=True,
    )
    with tempfile.TemporaryDirectory(prefix="stickwalker-check-") as temp:
        folder = Path(temp)
        linker = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
        for language, cases in (("c", C_TESTS), ("cpp", CPP_TESTS)):
            compiler = os.environ.get(
                "CC" if language == "c" else "CXX", "cc" if language == "c" else "c++"
            )
            for name, sources in cases.items():
                binary = folder / name
                command = [
                    compiler,
                    "-std=c11" if language == "c" else "-std=c++17",
                    "-DPW_STICK_S3",
                    "-g",
                    "-O1",
                    "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer",
                    "-ffunction-sections",
                    "-fdata-sections",
                    "-Iinclude",
                    "-Istick",
                    "-Istick/compat",
                    "-I.",
                    f"stick/tests/{name}_test.{language}",
                    *sources,
                    linker,
                    "-o",
                    str(binary),
                ]
                subprocess.run(command, check=True)
                subprocess.run([str(binary)], check=True)
                print(f"{name}: PASS", flush=True)
        for path in (
            "stick/tests/ir_tx_power_test.py",
            "stick/audits/m_only_wake_trace.py",
            "stick/audits/idle_power_trace.py",
            "stick/audits/abandoned_wake_trace.py",
            "stick/audits/codec_startup_trace.py",
        ):
            command = [sys.executable, path]
            if "codec_startup" in path:
                command += ["--output", str(folder / "codec.json")]
            try:
                result = subprocess.run(
                    command, check=True, capture_output=True, text=True
                )
            except subprocess.CalledProcessError as error:
                print(error.stdout, file=sys.stderr)
                print(error.stderr, file=sys.stderr)
                raise
            # Detailed audit reports stay available when a regression fails.
            if path.endswith(".py") and result.stdout.startswith("{"):
                json.loads(result.stdout)
            print(f"{Path(path).stem}: PASS", flush=True)
    print("Stickwalker host checks passed. Physical qualification is separate.")


if __name__ == "__main__":
    main()
