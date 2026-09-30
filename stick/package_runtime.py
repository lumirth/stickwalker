#!/usr/bin/env python3
"""Assemble portable release objects, linked SDK archives and dependency sources."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.common import InputError, digest, read_json, write_json


def retain_notices(source: Path, destination: Path):
    def wanted(name):
        return Path(name).name.lower().startswith(("license", "copying", "notice", "copyright"))

    def target(name):
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts:
            raise InputError(f"Invalid notice archive path: {name}")
        result = destination / relative
        result.parent.mkdir(parents=True, exist_ok=True)
        return result

    if source.suffix == ".zip":
        with zipfile.ZipFile(source) as archive:
            entries = (
                (name, archive.read(name))
                for name in archive.namelist()
                if wanted(name) and not name.endswith("/")
            )
            for name, data in entries:
                target(name).write_bytes(data)
    else:
        with tarfile.open(source) as archive:
            for entry in archive:
                if entry.isfile() and wanted(entry.name):
                    target(entry.name).write_bytes(archive.extractfile(entry).read())


def dependency_path(folder, name, notice=False):
    direct = folder / name
    return direct if direct.is_file() else folder / ("licenses" if notice else "sources") / name


def portable_arguments(arguments, build_path, sdk, toolchain):
    substitutions = (
        (str(build_path), "{kit}/objects"),
        (str(sdk), "{kit}/sdk"),
        (str(toolchain), "{toolchain}"),
    )
    result = []
    for argument in arguments:
        for original, replacement in substitutions:
            argument = argument.replace(original, replacement)
        if argument.startswith("-Wl,--Map="):
            argument = "-Wl,--Map={kit}/output/PwStick.ino.map"
        elif argument == "{kit}/objects/PwStick.ino.elf":
            argument = "{kit}/output/PwStick.ino.elf"
        # Every absolute input must belong to one of the supplied trees.
        if re.search(r"(?:^|[=@]|^-L)/", argument):
            raise InputError(f"Unbundled absolute link input: {argument}")
        result.append(argument)
    return result


def assemble(args):
    build = args.build_dir.resolve()
    output = args.output_dir.resolve()
    if output.exists() and any(output.iterdir()):
        raise InputError("Runtime output must be new or empty.")
    recipe = read_json(build / "link-command.json")
    manifest = read_json(build / "build-manifest.json")
    sdk = args.sdk.resolve()
    toolchain = args.toolchain.resolve()
    arguments = portable_arguments(recipe["arguments"], recipe["build_path"], sdk, toolchain)
    sources = args.sources_dir.resolve()
    provenance = read_json(sources / "sources.json")
    if provenance != read_json(ROOT / "stick/runtime-sources.json"):
        raise InputError("Dependency sources do not match the release source inventory.")
    for name, row in provenance["archives"].items():
        if digest(dependency_path(sources, name)) != row["sha256"]:
            raise InputError(f"Dependency source archive changed: {name}")
    for name, row in provenance["notices"].items():
        if digest(dependency_path(sources, name, notice=True)) != row["sha256"]:
            raise InputError(f"Dependency notice changed: {name}")
    output.mkdir(parents=True, exist_ok=True)
    for argument in arguments:
        if argument.startswith("{kit}/objects/") and argument.endswith((".o", ".a")):
            relative = Path(argument.removeprefix("{kit}/objects/"))
            destination = output / "objects" / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(build / "relink-build" / relative, destination)
    map_text = (build / "relink-build/PwStick.ino.map").read_text()
    used = set(re.findall(r"([^/\s]+\.a)\(", map_text))
    # Keep each referenced archive in its original search directory. Removing
    # unused -l entries avoids redistributing unrelated SDK libraries.
    for folder in ("flags", "ld", "qio_opi", "include"):
        shutil.copytree(sdk / folder, output / "sdk" / folder)
    for path in (sdk / "lib").glob("*.a"):
        if path.name in used:
            destination = output / "sdk/lib" / path.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
    for name in ("versions.txt", "sdkconfig"):
        shutil.copy2(sdk / name, output / "sdk" / name)
    libraries = output / "sdk/flags/ld_libs"
    toolchain_libraries = {"c", "m", "stdc++", "gcc", "xt_hal"}
    libraries.write_text(
        " ".join(
            token
            for token in libraries.read_text().split()
            if not token.startswith("-l")
            or token[2:] in toolchain_libraries
            or f"lib{token[2:]}.a" in used
        )
        + "\n"
    )
    shutil.copy2(ROOT / "stick/relink.py", output / "relink.py")
    shutil.copy2(ROOT / "stick/docs/relink.md", output / "README.md")
    for name in ("gcc", "newlib"):
        shutil.copytree(toolchain / "share/licenses" / name, output / "licenses/toolchain" / name)
    for path in (ROOT / "stick/licenses").iterdir():
        shutil.copy2(path, output / "licenses" / path.name)
    shutil.copy2(
        dependency_path(sources, "Arduino-LGPL-2.1.txt", notice=True),
        output / "licenses/Arduino-LGPL-2.1.txt",
    )
    for name in provenance["archives"]:
        destination = output / "sources" / name
        destination.parent.mkdir(exist_ok=True)
        shutil.copy2(dependency_path(sources, name), destination)
        retain_notices(destination, output / "licenses/sources" / name)
    write_json(output / "sources.json", provenance)
    write_json(output / "link.json", {"arguments": arguments, "toolchain": "esp-x32 2601"})
    subprocess.run(
        [sys.executable, str(output / "relink.py"), "--toolchain", str(toolchain)], check=True
    )
    if digest(output / "output/PwStick.ino.elf") != manifest["outputs"]["PwStick.ino.elf"]:
        raise InputError("Relinking did not reproduce the candidate ELF.")
    shutil.rmtree(output / "output")
    write_json(
        output / "runtime-manifest.json",
        {
            "elf_sha256": manifest["outputs"]["PwStick.ino.elf"],
            "app_sha256": manifest["outputs"]["PwStick.ino.bin"],
            "relink_verified": True,
            "referenced_archives": sorted(used),
            "files": {
                p.relative_to(output).as_posix(): digest(p)
                for p in sorted(output.rglob("*"))
                if p.is_file()
            },
        },
    )
    print(f"Runtime sources and verified relinking kit: {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("build-dir", "sdk", "toolchain", "sources-dir", "output-dir"):
        parser.add_argument("--" + option, type=Path, required=True)
    args = parser.parse_args()
    try:
        assemble(args)
    except (InputError, OSError, subprocess.SubprocessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
