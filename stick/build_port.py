#!/usr/bin/env python3
"""Build Stickwalker with the pinned M5Stack Arduino toolchain."""

from __future__ import annotations

import argparse
import hashlib
import json
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import assets
from tools.common import InputError, digest, read_json, write_json

FQBN = "m5stack:esp32:m5stack_sticks3:PSRAM=opi"
CORE = "3.3.9"
LIBRARIES = {"M5Unified": "0.2.21", "M5GFX": "0.2.29"}
EXCLUDE = {
    "src/application/pw_accel_bma150.c",  # BMI270-backed BMA150 register seam
    "src/application/pw_factory_test.c",  # H8-only fixture boot handshake
    "src/application/pw_ssu_init.c",  # H8 shared SPI bus setup
    "src/application/pw_eeprom_m95512_bus.c",  # Stick storage backend
    "src/startup/h8_dbsct.c",  # Renesas section table
    "src/startup/h8_intprg.c",  # Renesas vector table
}


def source_inputs() -> list[Path]:
    paths = [
        p
        for folder in ("src", "include")
        for p in (ROOT / folder).rglob("*")
        if p.is_file() and p.suffix in {".c", ".h"}
    ]
    paths += [
        p
        for folder in (ROOT / "stick", ROOT / "stick/compat")
        for p in folder.iterdir()
        if p.is_file() and p.suffix in {".c", ".cpp", ".h"}
    ]
    return sorted(
        paths
        + [
            ROOT / "stick/build_port.py",
            ROOT / "config/artwork.json",
            ROOT / "tools/assets.py",
            ROOT / "tools/common.py",
        ]
    )


def stage(sketch: Path, artwork: str) -> dict:
    (sketch / "src").mkdir(parents=True)
    (sketch / "PwStick.ino").write_text(
        "#include <Arduino.h>\n"
        'extern "C" void StickPortSetup(void);\n'
        'extern "C" void StickPortLoop(void);\n'
        "void setup() { StickPortSetup(); }\n"
        "void loop() { StickPortLoop(); }\n"
    )
    inputs = source_inputs()
    for source in inputs:
        rel = source.relative_to(ROOT)
        if rel.as_posix() in EXCLUDE or source.suffix not in {".c", ".cpp"}:
            continue
        if rel.parts[0] not in {"src", "stick"}:
            continue
        destination = sketch / "src" / rel
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    manifest = read_json(ROOT / "config/artwork.json")
    selected = assets.resolved_inputs(ROOT, manifest)
    if artwork == "placeholders":
        selected = {a["name"]: ROOT / "assets/placeholders" / a["file"] for a in manifest}
    lines = ["/* Generated from editable artwork files. */"]
    for asset in manifest:
        data = assets.encode(selected[asset["name"]], asset)
        lines += [f"/* {asset['name']} */", "{"]
        for offset in range(0, len(data), 12):
            lines.append("  " + ", ".join(f"0x{v:02X}" for v in data[offset : offset + 12]) + ",")
        lines.append("},")
    generated = ("\n".join(lines) + "\n").encode("ascii")
    (sketch / "rom_assets.h").write_bytes(generated)
    inputs += list(selected.values())
    return {
        "source_sha256": {p.relative_to(ROOT).as_posix(): digest(p) for p in inputs},
        "artwork": artwork,
        "rom_assets_sha256": hashlib.sha256(generated).hexdigest(),
    }


def build(args) -> None:
    cli = [args.arduino_cli]
    if args.config_file:
        cli += ["--config-file", str(args.config_file.resolve())]

    def query(*parts):
        return json.loads(
            subprocess.check_output(cli + list(parts) + ["--format", "json"], text=True)
        )

    version = query("version")
    core = next(
        (p for p in query("core", "list")["platforms"] if p["id"] == "m5stack:esp32"),
        {},
    )
    if core.get("installed_version") != CORE:
        raise InputError(f"Install m5stack:esp32@{CORE}; see stick/docs/build.md.")
    installed = {
        row["library"]["name"]: row["library"]["version"]
        for row in query("lib", "list")["installed_libraries"]
    }
    for name, expected in LIBRARIES.items():
        if installed.get(name) != expected:
            raise InputError(f"Install {name}@{expected}; see stick/docs/build.md.")
    output = args.output_dir.resolve()
    # Never leave a successful manifest attached to a failed/replaced build.
    output.mkdir(parents=True, exist_ok=True)
    (output / "build-manifest.json").unlink(missing_ok=True)
    workspace = ROOT / "stick/.build"
    workspace.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="compile-", dir=workspace) as temp:
        sketch = Path(temp) / "PwStick"
        manifest = stage(sketch, args.artwork)
        includes = [
            ROOT / "include",
            ROOT / "stick",
            ROOT,
            ROOT / "stick/compat",
            sketch,
        ]
        flags = "-DPW_STICK_S3 "
        if args.bench_control:
            flags += "-DPW_STICK_BENCH_CONTROL "
        flags += " ".join(f'"-I{path}"' for path in includes)
        command = cli + [
            "compile",
            "--fqbn",
            FQBN,
            "--output-dir",
            str(output),
            "--build-property",
            f"compiler.c.extra_flags={flags}",
            "--build-property",
            f"compiler.cpp.extra_flags={flags}",
            "--warnings",
            "all",
        ]
        if args.verbose or args.retain_build:
            command.append("--verbose")
        build_path = Path(temp) / "objects"
        command += ["--build-path", str(build_path)]
        if args.retain_build:
            result = subprocess.run(
                command + [str(sketch)],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            (output / "compile.log").write_text(result.stdout)
            if result.returncode:
                print(result.stdout)
                result.check_returncode()
            link = next(
                shlex.split(line)
                for line in result.stdout.splitlines()
                if "-Wl,--start-group" in line and "-o" in line
            )
            retained = output / "relink-build"
            if retained.exists():
                shutil.rmtree(retained)
            shutil.copytree(build_path, retained)
            write_json(
                output / "link-command.json",
                {
                    "arguments": link,
                    "build_path": str(build_path),
                },
            )
            print("Saved compiler output and relinking inputs.")
        else:
            subprocess.run(command + [str(sketch)], check=True, cwd=ROOT)
        shutil.copy2(build_path / "boot_app0.bin", output / "boot_app0.bin")
        manifest.update(
            {
                "fqbn": FQBN,
                "core": CORE,
                "libraries": LIBRARIES,
                "arduino_cli": version,
                "bench_control": args.bench_control,
                "outputs": {
                    p.name: digest(p)
                    for p in sorted([*output.glob("PwStick.ino.*"), output / "boot_app0.bin"])
                    if p.suffix in {".bin", ".elf"}
                },
            }
        )
        if manifest["source_sha256"] != {p: digest(ROOT / p) for p in manifest["source_sha256"]}:
            raise InputError("Inputs changed during compilation; build again.")
        write_json(output / "build-manifest.json", manifest)
        print(f"Build and input hashes: {output / 'build-manifest.json'}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--config-file", type=Path)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "stick/.build/output")
    parser.add_argument("--artwork", choices=("placeholders", "local"), default="placeholders")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument(
        "--retain-build",
        action="store_true",
        help="Keep objects and the link command for release relinking materials.",
    )
    parser.add_argument("--bench-control", action="store_true")
    args = parser.parse_args()
    try:
        build(args)
    except (InputError, OSError, subprocess.SubprocessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
