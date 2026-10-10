#!/usr/bin/env python3
"""Exercise identity persistence and native peer lifecycle with simulated I/O."""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    "stick/tests/peer_lifecycle_test.c",
    "stick/device_identity.c",
    "src/application/pw_storage.c",
    "src/application/pw_eeprom_m95512_io.c",
    "src/application/pw_friend.c",
    "src/application/pw_rtc.c",
    "src/application/pw_selftest.c",
    "src/application/pw_selftest_data.c",
    "src/startup/h8_resetprg.c",
    "src/support/lib_clear.c",
    "src/support/ir.c",
]


def main():
    compiler = os.environ.get("CC", "cc")
    common = [
        compiler,
        "-std=c11",
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
    ]
    with tempfile.TemporaryDirectory(prefix="stickwalker-peer-") as folder:
        objects = []
        for i, source in enumerate(SOURCES):
            obj = str(Path(folder) / f"{i}.o")
            objects.append(obj)
            # Replace only the UI callback; retain protocol completion logic.
            flags = ["-DIrComplete=NativeIrComplete"] if source.endswith("h8_resetprg.c") else []
            subprocess.run([*common, *flags, "-c", source, "-o", obj], cwd=ROOT, check=True)
        binary = str(Path(folder) / "peer-lifecycle")
        linker = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
        subprocess.run(
            [compiler, "-fsanitize=address,undefined", *objects, linker, "-o", binary],
            cwd=ROOT,
            check=True,
        )
        subprocess.run([binary], cwd=ROOT, check=True)
        storage = str(Path(folder) / "eeprom-identity")
        subprocess.run(
            [
                os.environ.get("CXX", "c++"),
                "-std=c++17",
                "-g",
                "-O1",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-ffunction-sections",
                "-fdata-sections",
                "-Istick",
                "-Istick/tests/storage_fakes",
                "stick/tests/eeprom_identity_test.cpp",
                objects[1],
                linker,
                "-o",
                storage,
            ],
            cwd=ROOT,
            check=True,
        )
        subprocess.run([storage], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
