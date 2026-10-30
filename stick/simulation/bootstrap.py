"""Fetch and build the pinned ESP32-S3 simulator."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / "stick/.build/simulator"
SOURCE = CACHE / "esp32sim"
REVISION = "fe3a9c08125cc760107865df3ffa979362952fe9"
PATCH = Path(__file__).with_name("soc.patch")
MANIFEST = CACHE / "prepared.json"


def run(*args: str, cwd: Path = ROOT) -> str:
    return subprocess.check_output(args, cwd=cwd, text=True)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepared_record(binary: Path, rom: Path) -> dict:
    sources = (
        "bootstrap.py",
        "board.rs",
        "main.rs",
        "peer.rs",
        "Cargo.toml",
        "Cargo.lock",
        "soc.patch",
    )
    return {
        "system": platform.system(),
        "machine": platform.machine(),
        "revision": REVISION,
        "sources": {name: digest(PATCH.parent / name) for name in sources},
        "binary": digest(binary),
        "rom": digest(rom),
    }


def prepare(*, reuse: bool = False) -> tuple[Path, Path]:
    target = CACHE / "target"
    roms = CACHE / "roms"
    binary = target / "release/stickwalker-e2e"
    rom = roms / "esp32s3_rev0_rom.elf"
    if reuse:
        if json.loads(MANIFEST.read_text()) != prepared_record(binary, rom):
            raise RuntimeError(
                "Prepared simulator does not match its sources, platform, or binaries"
            )
        return binary, rom

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
    subprocess.run([str(SOURCE / "tools/fetch-rom-elfs.sh"), str(roms)], check=True)
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
    MANIFEST.write_text(json.dumps(prepared_record(binary, rom), indent=2) + "\n")
    return binary, rom


if __name__ == "__main__":
    prepare()
