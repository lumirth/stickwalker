"""Fetch and build the pinned ESP32-S3 simulator."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / "stick/.build/simulator"
SOURCE = CACHE / "esp32sim"
REVISION = "fe3a9c08125cc760107865df3ffa979362952fe9"
PATCH = Path(__file__).with_name("soc.patch")


def run(*args: str, cwd: Path = ROOT) -> str:
    return subprocess.check_output(args, cwd=cwd, text=True)


def prepare() -> tuple[Path, Path]:
    CACHE.mkdir(parents=True, exist_ok=True)
    if not SOURCE.exists():
        run(
            "git",
            "clone",
            "--filter=blob:none",
            "--no-checkout",
            "https://github.com/mikroverk/esp32sim.git",
            str(SOURCE),
        )
        run("git", "checkout", "--detach", REVISION, cwd=SOURCE)
    if run("git", "rev-parse", "HEAD", cwd=SOURCE).strip() != REVISION:
        raise RuntimeError(f"Simulator checkout differs from {REVISION}: {SOURCE}")
    diff = subprocess.check_output(["git", "diff", "--binary"], cwd=SOURCE)
    if not diff:
        run("git", "apply", "--check", str(PATCH), cwd=SOURCE)
        run("git", "apply", str(PATCH), cwd=SOURCE)
    elif diff != PATCH.read_bytes():
        raise RuntimeError(f"Simulator contains changes beyond soc.patch: {SOURCE}")
    roms = CACHE / "roms"
    subprocess.run([str(SOURCE / "tools/fetch-rom-elfs.sh"), str(roms)], check=True)
    target = CACHE / "target"
    subprocess.run(
        [
            "cargo",
            "build",
            "--release",
            "--locked",
            "--manifest-path",
            str(ROOT / "stick/simulation/Cargo.toml"),
        ],
        env={**os.environ, "CARGO_TARGET_DIR": str(target)},
        check=True,
    )
    return target / "release/stickwalker-e2e", roms / "esp32s3_rev0_rom.elf"


if __name__ == "__main__":
    prepare()
