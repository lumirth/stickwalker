#!/usr/bin/env python3
"""Package a committed production build without saves, retail artwork or diagnostics."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.common import InputError, digest, read_json, write_json
from stick.build_port import CORE, FQBN, LIBRARIES, source_inputs

REQUIRED = (
    "PwStick.ino.bin",
    "PwStick.ino.elf",
    "PwStick.ino.bootloader.bin",
    "PwStick.ino.partitions.bin",
    "boot_app0.bin",
)
DIAGNOSTICS = (
    b"PW_STICK_READY",
    b"PW_STICK_POWER_HW",
    b"PW_STICK_EEPROM",
    b"PW_STICK_IR RX",
    b"PW_STICK_WAKE",
)


def validate_build(folder: Path) -> dict:
    manifest = read_json(folder / "build-manifest.json")
    if (
        manifest.get("bench_control") is not False
        or manifest.get("artwork") != "placeholders"
    ):
        raise InputError(
            "Release firmware must be production mode with placeholder artwork."
        )
    if (manifest.get("fqbn"), manifest.get("core"), manifest.get("libraries")) != (
        FQBN,
        CORE,
        LIBRARIES,
    ):
        raise InputError("Build does not use the release board/core/library versions.")
    expected = {p.relative_to(ROOT).as_posix() for p in source_inputs()}
    expected.update(
        f"assets/placeholders/{a['file']}"
        for a in read_json(ROOT / "config/artwork.json")
    )
    if set(manifest.get("source_sha256", {})) != expected:
        raise InputError(
            "Build input inventory is incomplete or contains local artwork."
        )
    for name, sha in manifest["source_sha256"].items():
        if digest(ROOT / name) != sha:
            raise InputError(f"Build is stale: {name} changed; rebuild.")
    for name in REQUIRED:
        if digest(folder / name) != manifest.get("outputs", {}).get(name):
            raise InputError(f"Missing or changed output: {name}.")
    app = (folder / "PwStick.ino.bin").read_bytes()
    if len(app) > 0x330000 or any(marker in app for marker in DIAGNOSTICS):
        raise InputError("Application exceeds app0 or contains bench diagnostics.")
    timing = read_json(folder / "ir-timing.json")
    baseline = read_json(ROOT / "stick/ir-timing-baseline.json")
    expected_functions = baseline["functions"]
    if (
        timing.get("elf_sha256") != manifest["outputs"]["PwStick.ino.elf"]
        or timing.get("reference_app_sha256") != baseline["reference_app_sha256"]
        or set(timing.get("functions", {})) != set(expected_functions)
        or not timing.get("passed")
    ):
        raise InputError("Missing or stale IR timing verification.")
    for name, expected in expected_functions.items():
        row = timing["functions"][name]
        if (
            row.get("bytes") != expected["bytes"]
            or row.get("sha256") != expected["sha256"]
        ):
            raise InputError(
                f"IR timing changed: {name}; physical requalification is required."
            )
    return manifest


def package(args) -> None:
    def git(*parts):
        return subprocess.check_output(["git", "-C", str(ROOT), *parts])

    if git("status", "--porcelain", "--untracked-files=normal").strip():
        raise InputError(
            "Commit the intended source and remove untracked release inputs first."
        )
    commit = git("rev-parse", "HEAD").decode().strip()
    manifest = validate_build(args.build_dir.resolve())
    output = args.output_dir.resolve()
    if output.exists() and any(output.iterdir()):
        raise InputError(
            "Output directory must be new or empty; preserve prior candidates."
        )
    output.mkdir(parents=True, exist_ok=True)
    prefix = f"stickwalker-{commit[:12]}"
    source = output / f"{prefix}-source.tar.gz"
    with source.open("wb") as stream:
        subprocess.run(
            [
                "git",
                "-C",
                str(ROOT),
                "archive",
                "--format=tar.gz",
                f"--prefix={prefix}/",
                commit,
            ],
            stdout=stream,
            check=True,
        )
    with tempfile.TemporaryDirectory(prefix="stickwalker-package-") as temp:
        kit = Path(temp)
        for name in (*REQUIRED, "ir-timing.json"):
            shutil.copy2(args.build_dir / name, kit / name)
        manifest.update(
            {
                "git_commit": commit,
                "hardware_qualified": False,
                "qualification": "See RELEASE.md; qualify these exact output hashes.",
            }
        )
        write_json(kit / "build-manifest.json", manifest)
        for original, name in (
            ("LICENSE", "LICENSE"),
            ("stick/docs/install.md", "INSTALL.md"),
            ("stick/docs/release.md", "RELEASE.md"),
            ("docs/third-party.md", "THIRD-PARTY.md"),
        ):
            text = (ROOT / original).read_text()

            def link(match):
                label, target = match.groups()
                if target.startswith(("https://", "http://", "#")):
                    return match.group(0)
                local, _, anchor = target.partition("#")
                source_target = (ROOT / original).parent / local
                rel = source_target.resolve().relative_to(ROOT).as_posix()
                bundled = {
                    "stick/docs/install.md": "INSTALL.md",
                    "stick/docs/release.md": "RELEASE.md",
                    "docs/third-party.md": "THIRD-PARTY.md",
                    "LICENSE": "LICENSE",
                }
                destination = bundled.get(
                    rel, f"https://github.com/lumirth/stickwalker/blob/{commit}/{rel}"
                )
                if anchor:
                    destination += "#" + anchor
                return f"[{label}]({destination})"

            text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", link, text)
            (kit / name).write_text(text)
        licenses = ROOT / "stick/licenses"
        for original in licenses.rglob("*"):
            if original.is_file():
                destination = kit / "licenses" / original.relative_to(licenses)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(original, destination)
        (kit / "SHA256SUMS").write_text(
            "".join(
                f"{digest(p)}  {p.relative_to(kit).as_posix()}\n"
                for p in sorted(kit.rglob("*"))
                if p.is_file()
            )
        )
        firmware = output / f"{prefix}-firmware.zip"
        with zipfile.ZipFile(
            firmware, "w", compression=zipfile.ZIP_DEFLATED
        ) as archive:
            for path in sorted(kit.rglob("*")):
                if path.is_file():
                    archive.write(path, path.relative_to(kit).as_posix())
    (output / "SHA256SUMS").write_text(
        f"{digest(source)}  {source.name}\n{digest(firmware)}  {firmware.name}\n"
    )
    print(f"Candidate prepared at {output}; hardware qualification remains open.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    try:
        package(args)
    except (InputError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
