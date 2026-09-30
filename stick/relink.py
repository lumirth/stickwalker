#!/usr/bin/env python3
"""Relink the release objects, optionally replacing the Arduino core archive."""

import argparse
import json
import subprocess
from pathlib import Path


def command(arguments, root, toolchain, core=None):
    replacements = {"{kit}": str(root), "{toolchain}": str(toolchain)}
    result = []
    for argument in arguments:
        for token, value in replacements.items():
            argument = argument.replace(token, value)
        if core and argument == str(root / "objects/core/core.a"):
            argument = str(core)
        result.append(argument)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--toolchain",
        type=Path,
        required=True,
        help="Root of the esp-x32 2601 toolchain, containing bin/.",
    )
    parser.add_argument(
        "--core", type=Path, help="Replacement core.a built for the same board/configuration."
    )
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    recipe = json.loads((root / "link.json").read_text())
    (root / "output").mkdir(exist_ok=True)
    subprocess.run(
        command(
            recipe["arguments"],
            root,
            args.toolchain.resolve(),
            args.core.resolve() if args.core else None,
        ),
        cwd=root,
        check=True,
    )
    print(f"Relinked ELF: {root / 'output/PwStick.ino.elf'}")


if __name__ == "__main__":
    main()
