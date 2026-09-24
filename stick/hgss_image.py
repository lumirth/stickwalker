#!/usr/bin/env python3
"""Build the HGSS Pokewalker entry image from the game's original source assets.

This is a bench input generator. The image still travels through the 3DS IR
peer and the Pokewalker's original packet and storage code; it is not injected
into Stick storage. No HGSS assets are copied into this repository.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import re
import subprocess
import tempfile
from pathlib import Path


def arithmetic(expression: str, names: dict[str, int] | None = None) -> int:
    expression = re.sub(r"\b([0-9]+)\b", r"\1", expression.strip())
    node = ast.parse(expression, mode="eval").body

    def visit(value: ast.AST) -> int:
        if isinstance(value, ast.Constant) and type(value.value) is int:
            return value.value
        if isinstance(value, ast.Name) and names and value.id in names:
            return names[value.id]
        if isinstance(value, ast.BinOp):
            a, b = visit(value.left), visit(value.right)
            if isinstance(value.op, ast.Add):
                return a + b
            if isinstance(value.op, ast.Sub):
                return a - b
            if isinstance(value.op, ast.Mult):
                return a * b
            if isinstance(value.op, (ast.Div, ast.FloorDiv)):
                return a // b
            if isinstance(value.op, ast.BitOr):
                return a | b
        raise ValueError(f"unsupported source expression: {expression}")

    return visit(node)


def image_offsets(struct_header: Path, fields: set[str]) -> dict[str, tuple[int, int]]:
    source = struct_header.read_bytes().decode("latin1")
    start = source.index("typedef struct{", source.index("}PHCStatus;"))
    end = source.index("}PHCImageData;", start) + len("}PHCImageData;")
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        (directory / "image.h").write_text(
            "typedef unsigned char u8;\ntypedef unsigned short u16;\n"
            "#define UI_SOUND_3 1\n" + source[start:end] + "\n"
        )
        c = ['#include <stdio.h>', '#include <stddef.h>', '#include "image.h"',
             'int main(void) {', 'printf("TOTAL %zu\\n", sizeof(PHCImageData));']
        for field in sorted(fields):
            c.append(f'printf("{field} %zu %zu\\n", offsetof(PHCImageData, {field}), '
                     f'sizeof(((PHCImageData *)0)->{field}));')
        c.append("}")
        (directory / "offsets.c").write_text("\n".join(c))
        subprocess.run(["cc", "-I", str(directory), str(directory / "offsets.c"),
                        "-o", str(directory / "offsets")], check=True)
        lines = subprocess.check_output([str(directory / "offsets")], text=True).splitlines()
    assert lines[0] == "TOTAL 35920", lines[0]
    return {row[0]: (int(row[1]), int(row[2])) for line in lines[1:]
            if (row := line.split())}


def asset_bytes(source: Path, symbol: str) -> bytes:
    matches = list(source.rglob(f"{symbol.rsplit('_', 1)[0]}.h"))
    if len(matches) != 1:
        raise ValueError(f"expected one header for {symbol}, got {matches}")
    text = matches[0].read_bytes().decode("latin1")
    match = re.search(rf"\b{symbol}\s*\[[^]]+\]\s*=\s*\{{(.*?)\}}\s*;", text, re.S)
    if not match:
        raise ValueError(f"missing {symbol} in {matches[0]}")
    body = re.sub(r"//[^\n]*|/\*.*?\*/", "", match.group(1), flags=re.S)
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{1,2})\b", body))


def sound_bytes(source: Path) -> bytes:
    sound_source = (source / "src/application/phc_link/phc_sounddata.c").read_bytes().decode("latin1")
    header = (source / "include/phc/phc_beep.h").read_bytes().decode("latin1")
    names: dict[str, int] = {}
    for name, value in re.findall(r"^#define\s+(NOTE_\w+|SCALE_MASK)\s+([^/\n]+)", header, re.M):
        try:
            names[name] = arithmetic(value, names)
        except (ValueError, SyntaxError):
            pass
    order_match = re.search(r"static const Note \* const note_tbl\[\]\s*=\s*\{(.*?)\};",
                            sound_source, re.S)
    assert order_match
    order = re.findall(r"\b(?:note_\w+|item_get)\b", order_match.group(1))
    assert len(order) == 16, order
    output = bytearray(544)
    cursor = 0
    for index, name in enumerate(order):
        matches = list(re.finditer(rf"static const Note {name}\[\]\s*=\s*\{{(.*?)\}};",
                                   sound_source, re.S))
        assert len(matches) == 1, name
        body = re.sub(r"//[^\n]*|/\*.*?\*/", "", matches[0].group(1), flags=re.S)
        body = re.sub(r"#if 0\b.*?#endif", "", body, flags=re.S)
        body = re.sub(r"^#(?:if|else|endif).*?$", "", body, flags=re.M)
        notes = re.findall(r"\{\s*([^,{}]+)\s*,\s*([^,{}]+)\s*,?\s*\}", body)
        assert notes, name
        score = bytes(value & 255 for pair in notes
                      for value in (arithmetic(pair[0], names), arithmetic(pair[1], names)))
        assert len(score) <= 192 and cursor + len(score) <= 480, name
        output[index * 4:index * 4 + 2] = cursor.to_bytes(2, "little")
        output[index * 4 + 2] = len(score)
        output[index * 4 + 3] = sum(score) & 255
        output[64 + cursor:64 + cursor + len(score)] = score
        cursor += len(score)
    return bytes(output)


def build(source: Path) -> tuple[bytes, dict]:
    link = (source / "src/application/phc_link/phclink.c").read_bytes().decode("latin1")
    begin = link.index("static void set_image( PHCLINK_WORK *wk, PHCImageData *myImageData )",
                       link.index("static void set_image( PHCLINK_WORK *wk, PHCImageData *myImageData )") + 1)
    end = link.index("setup_sound( &myImageData->soundData)", begin)
    fragment = re.sub(r"//[^\n]*|/\*.*?\*/", "", link[begin:end], flags=re.S)
    copies = re.findall(r"MI_CpuCopy8\s*\(\s*(phc_\w+|buf)\s*,\s*"
                        r"&myImageData->(\w+)\[(.*?)\]\s*,\s*(.*?)\s*\)\s*;",
                        fragment, re.S)
    offsets = image_offsets(source / "include/phc/phc_struct.h",
                            {field for _, field, _, _ in copies} | {"soundData"})
    image = bytearray(35920)
    copied = []
    for symbol, field, index, length in copies:
        if symbol == "buf":  # Trainer name is rendered dynamically from save data.
            continue
        relative, field_size = offsets[field]
        start = arithmetic(index)
        size = arithmetic(length)
        # The retail source has a few copies that cross a declared field. The
        # later copies in set_image overwrite those bytes; retain its order.
        assert relative + start + size <= len(image), (symbol, field, start, size)
        data = asset_bytes(source / "src/application/phc_link", symbol)
        if symbol == "phc_mes_69_80x16":
            # HGSS copies 384 bytes from a 320-byte asset. The extra 64 bytes
            # have no declared source pixels and are unused by pw's 80x16 prompt.
            data += bytes(64)
        assert len(data) >= size, (symbol, len(data), size)
        image[relative + start:relative + start + size] = data[:size]
        copied.append(symbol)
    sound_offset, sound_length = offsets["soundData"]
    sound = sound_bytes(source)
    assert len(sound) == sound_length, (len(sound), sound_length)
    image[sound_offset:sound_offset + sound_length] = sound
    return bytes(image), {"assets": copied, "sound_score_bytes": sum(sound[2:64:4]),
                          "sha256": hashlib.sha256(image).hexdigest()}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("hgss_source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    image, manifest = build(args.hgss_source / "pokemon_gs 2")
    args.output.write_bytes(image)
    args.output.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"{len(image)} bytes, {len(manifest['assets'])} assets, {manifest['sha256']}")


if __name__ == "__main__":
    main()
