#!/usr/bin/env python3
"""Extract the seven native resident artwork arrays from a matching local ROM.

The offsets come from lumirth/pw's artwork configuration.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parents[1]
ROM_SIZE = 49152
ROM_SHA256 = "f9e210a3b74afbbd12c5a66a51cc05cb9fbac986805ff0a3bfb4be6074d15607"
ASSETS = (
    ("walkerImage", 47988, 256),
    ("neutralFace", 48244, 32),
    ("smileFace", 48276, 32),
    ("frownFace", 48308, 32),
    ("buttonArrow", 48340, 16),
    ("irSignal", 48356, 16),
    ("alphanumericFont", 48372, 108),
)


def atomic_write(path: Path, data: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", newline="\n", dir=path.parent,
            prefix=f".{path.name}.", delete=False,
        ) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path, help="matching 49,152-byte Pokewalker firmware image")
    parser.add_argument(
        "--output", type=Path,
        default=ROOT / "assets" / "local" / "rom_assets.h",
        help="generated native header (default: assets/local/rom_assets.h)",
    )
    args = parser.parse_args()
    try:
        rom = args.rom.read_bytes()
    except OSError as error:
        parser.error(str(error))
    if len(rom) != ROM_SIZE:
        parser.error(
            f"firmware is {len(rom):,} bytes; this reconstruction requires "
            f"exactly {ROM_SIZE:,} bytes"
        )
    digest = hashlib.sha256(rom).hexdigest()
    if digest != ROM_SHA256:
        parser.error(
            "firmware SHA-256 does not match the authoritative reconstruction; "
            "no output was written\n"
            f"expected: {ROM_SHA256}\nactual:   {digest}"
        )

    lines = [f"/* Pokewalker resident artwork from ROM {digest}. */"]
    for name, offset, size in ASSETS:
        data = rom[offset:offset + size]
        lines.extend((f"/* {name}, ROM 0x{offset:04X} */", "{"))
        for row in range(0, size, 12):
            lines.append("  " + ", ".join(f"0x{value:02X}" for value in data[row:row + 12]) + ",")
        lines.append("},")

    output = args.output.expanduser().resolve()
    atomic_write(output, "\n".join(lines) + "\n")
    print(f"Build with -DSW_RESIDENT_ASSETS_HEADER={output}")


if __name__ == "__main__":
    main()
