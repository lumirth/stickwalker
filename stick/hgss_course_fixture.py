#!/usr/bin/env python3
"""Build a coherent first-course PHC_Put fixture from the HGSS source tree.

This is bench game data, not a replacement for the original PHC protocol engine.
The encounter/item tables and graphics come from the game source. A trainer's
actual save, random encounter selection, and localized message renderer are not
available in this bench, so those choices are stated in the manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from hgss_image import asset_bytes


def first_course_table(path: Path) -> list:
    text = path.read_bytes().decode("cp932")
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    start = text.index("PhcCourseData[]")
    text = text[text.index("{", start):]
    tokens = re.findall(r"\{|\}|\d+|ADVANTAGE_\w+", text)
    position = 0

    def parse() -> list:
        nonlocal position
        assert tokens[position] == "{"
        position += 1
        result = []
        while tokens[position] != "}":
            if tokens[position] == "{":
                result.append(parse())
            else:
                value = tokens[position]
                result.append(int(value) if value.isdecimal() else value)
                position += 1
        position += 1
        return result

    table = parse()
    return table[0]


def narc_entry(path: Path, index: int) -> bytes:
    archive = path.read_bytes()
    assert archive[:4] == b"NARC" and archive[16:20] == b"BTAF"
    count = int.from_bytes(archive[24:26], "little")
    assert 0 <= index < count
    start = int.from_bytes(archive[28 + index * 8:32 + index * 8], "little")
    end = int.from_bytes(archive[32 + index * 8:36 + index * 8], "little")
    section = archive.index(b"GMIF") + 8
    encoded = archive[section + start:section + end]
    assert encoded[0] == 0x10  # Nintendo LZ10
    output_size = int.from_bytes(encoded[1:4], "little")
    output = bytearray()
    cursor = 4
    while len(output) < output_size:
        flags = encoded[cursor]
        cursor += 1
        for bit in range(7, -1, -1):
            if len(output) == output_size:
                break
            if flags & (1 << bit):
                a, b = encoded[cursor:cursor + 2]
                cursor += 2
                length, distance = (a >> 4) + 3, (((a & 15) << 8) | b) + 1
                assert distance <= len(output)
                for _ in range(length):
                    output.append(output[-distance])
            else:
                output.append(encoded[cursor])
                cursor += 1
    assert len(output) == output_size
    return bytes(output)


def sprite_index(path: Path, species: int, sex: str = "m") -> int:
    source = path.read_text(errors="replace")
    symbol = rf"NARC_phcgra_pmdp_{species:03d}_{sex}_lz_dat\s*=\s*(\d+)"
    match = re.search(symbol, source)
    assert match, (species, sex)
    return int(match.group(1))


def label(text: str, width: int) -> bytes:
    # PHC's source bmp2phc output: for each 8-pixel vertical strip, one byte
    # for high color bits and one for low color bits per x coordinate.
    image = Image.new("1", (width, 16), 0)
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 12)
    draw.text((2, 0), text, font=font, fill=1)
    output = bytearray(width * 16 // 4)
    index = 0
    for strip in range(2):
        for x in range(width):
            for y in range(8):
                if image.getpixel((x, strip * 8 + y)):
                    output[index] |= 1 << y
                    output[index + 1] |= 1 << y
            index += 2
    return bytes(output)


def pokemon(target: bytearray, offset: int, row: list, *, walking: bool = False) -> None:
    species, level, item, form, sex, moves, *_ = row
    target[offset:offset + 2] = species.to_bytes(2, "little")
    target[offset + 2:offset + 4] = item.to_bytes(2, "little")
    for i, move in enumerate(moves):
        target[offset + 4 + i * 2:offset + 6 + i * 2] = move.to_bytes(2, "little")
    target[offset + 12] = level
    target[offset + 13] = (form & 31) | ((sex & 3) << 5)
    # reverseFlag comes from personal data in set_course. It is zero for the
    # chosen normal-form species; rarity and egg are also zero in this fixture.
    if walking:
        assert species == 152 and form == 0


def build(source: Path) -> tuple[bytes, dict]:
    course_root = source / "src/application/phc_link"
    graphic_root = source / "src/graphic"
    course_data = first_course_table(course_root / "course/coursedata.dat")
    _, graphic_id, encounters, items, _ = course_data
    assert graphic_id == 1 and len(encounters) == 6 and len(items) == 10
    selected = [encounters[i] for i in (0, 2, 4)]
    # Chikorita is a legal HGSS starter and a plausible early walking partner.
    # No actual trainer save is represented by this fixture.
    walker = [152, 5, 0, 0, 0, [33, 45, 0, 0], 0, 0]
    result = bytearray(10430)
    pokemon(result, 0, walker, walking=True)
    result[38] = 70  # representative friendship, as the save would provide
    result[39] = graphic_id - 1
    for slot, row in enumerate(selected):
        pokemon(result, 82 + slot * 16, row)
        result[130 + slot * 2:132 + slot * 2] = row[6].to_bytes(2, "little")
        result[136 + slot] = row[7]
    for slot, row in enumerate(items):
        item_id, steps, odds = row
        result[140 + slot * 2:142 + slot * 2] = item_id.to_bytes(2, "little")
        result[160 + slot * 2:162 + slot * 2] = steps.to_bytes(2, "little")
        result[180 + slot] = odds
    def section(offset: int, data: bytes, expected: int) -> None:
        assert len(data) == expected, (offset, len(data), expected)
        result[offset:offset + expected] = data
    section(190, asset_bytes(course_root, "phc_course_01_32x24"), 192)
    section(382, label("Refreshing Field", 80), 320)
    icon = graphic_root / "phcicon.narc"
    big = graphic_root / "phcgra.narc"
    section(702, narc_entry(icon, 152), 384)
    section(1086, narc_entry(big, sprite_index(course_root / "phcgra.naix", 152)), 1536)
    section(2622, label("CHIKORITA", 80), 320)
    names = ("KANGASKHAN", "NIDORAN F", "PIDGEY")
    for slot, (row, name) in enumerate(zip(selected, names)):
        section(2942 + slot * 384, narc_entry(icon, row[0]), 384)
        section(5630 + slot * 320, label(name, 80), 320)
    section(4094, narc_entry(big, sprite_index(course_root / "phcgra.naix", selected[2][0])), 1536)
    item_names = ("REVIVE", "FULL HEAL", "BURN HEAL", "ICE HEAL", "CHESTO BERRY",
                  "AWAKENING", "CHERI BERRY", "PARLYZ HEAL", "ORAN BERRY", "POTION")
    for slot, name in enumerate(item_names):
        section(6590 + slot * 384, label(name, 96), 384)
    manifest = {
        "source_course_index": 0, "source_graphic_id": graphic_id,
        "selected_encounter_table_indices": [0, 2, 4],
        "walking_pokemon": "Chikorita, level 5, representative starter fixture",
        "message_graphics": "English labels rendered for the bench; not DS message renderer output",
        "record": "zeroed DSRecordData; no real trainer save exists in this bench",
        "sha256": hashlib.sha256(result).hexdigest(),
    }
    return bytes(result), manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    course, manifest = build(args.source / "pokemon_gs 2")
    args.output.write_bytes(course)
    args.output.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(manifest)


if __name__ == "__main__":
    main()
