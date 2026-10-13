"""Public synthetic EEPROM: native record layouts, original raster labels and score.

Addresses and checksums follow include/eeprom_map.h, resources.h, records.h,
save.h, score.h and the matching firmware's EEPROM/score loaders. No save or
uploaded game artwork is imported. The legacy envelope exercises migration.
"""

import struct
import subprocess
import zlib
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def raster(text: str, width: int, height: int = 16) -> bytes:
    image = Image.new("1", (width, height))
    draw = ImageDraw.Draw(image)
    font = ImageFont.load_default(size=9)
    draw.text((1, 2), text, font=font, fill=1)
    data = bytearray()
    for page in range(0, height, 8):
        for x in range(width):
            column = sum(bool(image.getpixel((x, page + bit))) << bit for bit in range(8))
            data.extend((column, column))
    return bytes(data)


def eeprom(watts: int = 100) -> bytes:
    image = bytearray(65536)
    image[:8] = b"nintendo"

    def mirror(primary: int, backup: int, payload: bytes) -> None:
        record = payload + bytes([(1 + sum(payload)) & 255])
        for address in (primary, backup):
            image[address : address + len(record)] = record

    identity = b"PUBLIC SIMULATOR FIXTURE".ljust(40, b"\0")
    mirror(0x83, 0x183, identity)
    mirror(0x80, 0x180, bytes(2))
    mirror(0xAC, 0x1AC, bytes(64))
    mirror(0x16F, 0x26F, bytes(1))
    status = bytearray(104)
    status[16:56] = identity
    status[91] = 1
    status[95] = 2
    mirror(0xED, 0x1ED, status)
    save = bytearray(24)
    struct.pack_into(">I", save, 8, 0x0D2B0B80)
    struct.pack_into(">H", save, 14, watts)
    save[23] = (2 << 1) | (4 << 3)
    mirror(0x156, 0x256, save)

    def ui(offset: int, payload: bytes) -> None:
        image[0x280 + offset : 0x280 + offset + len(payload)] = payload

    for index, digit in enumerate("0123456789"):
        ui(index * 32, raster(digit, 8))
    for index, label in enumerate(
        ("RADAR", "DOWSING", "CONNECT", "TRAINER", "INVENTORY", "SETTINGS")
    ):
        ui(1680 + index * 320, raster(label, 80))
        ui(3600 + index * 64, raster(str(index + 1), 16))
    ui(5136, raster("VOL", 40))
    ui(5296, raster("LCD", 40))
    for index, label in enumerate(("OFF", "LOW", "MAX")):
        ui(5456 + index * 96, raster(label, 24))
    ui(5744, raster("X", 8))
    for direction in range(4):
        for variant in range(3):
            ui(632 + (direction * 3 + variant) * 16, bytes([0x3C, 0x3C] * 8))
    for index in range(68):
        ui(8880 + index * 384, raster(f"MESSAGE {index}", 96))
    score = bytes((120, 0x7B, 6, 20, 0, 0x7F))
    for index in range(16):
        struct.pack_into("<HBB", image, 0x8CB0 + index * 4, 0, len(score), sum(score) & 255)
    image[0x8CF0 : 0x8CF0 + len(score)] = score
    return bytes(image)


def filesystem(
    tool: Path, partitions: Path, folder: Path, case: str = "registered"
) -> tuple[int, Path]:
    table = partitions.read_bytes()
    partition = None
    for position in range(0, len(table), 32):
        magic, kind, subtype, offset, size, label, flags = struct.unpack_from(
            "<HBBII16sI", table, position
        )
        if magic != 0x50AA:
            break
        if kind == 1 and subtype == 0x82:
            partition = offset, size
    if partition is None:
        raise ValueError("Firmware has no LittleFS partition")
    offset, size = partition
    payload = eeprom(100 if case == "registered" else 200)
    source = folder / "fixture-files"
    source.mkdir()

    def versioned(magic: int, generation: int, sequence: int, data: bytes) -> bytes:
        header = struct.pack("<5I", magic, generation, sequence, len(data), zlib.crc32(data))
        return header + struct.pack("<I", zlib.crc32(header)) + data

    if case in ("registered", "recovery-legacy"):
        (source / "pw-a.bin").write_bytes(
            struct.pack("<III", 0x50574531, 1, zlib.crc32(payload)) + payload
        )
    else:
        (source / "pw2-a.bin").write_bytes(versioned(0x50574532, 2, 2, payload))
    if case != "registered":
        older = eeprom(100)
        patches = b"".join(
            struct.pack("<HH", address, 25) + older[address : address + 25]
            for address in (0x156, 0x256)
        )
        if case == "recovery-legacy":
            record = struct.pack("<III", 0x50574A31, len(patches), zlib.crc32(patches)) + patches
            (source / "pw-journal.bin").write_bytes(record)
        elif case == "recovery-stale":
            (source / "pw2-journal.bin").write_bytes(versioned(0x50574A32, 1, 1, patches))
        elif case == "recovery-truncated":
            (source / "pw2-journal.bin").write_bytes(versioned(0x50574A32, 2, 3, patches)[:-3])
        else:
            raise ValueError(f"Unknown fixture {case}")
    (folder / "fixture.eeprom").write_bytes(payload)
    target = folder / "fixture.littlefs"
    subprocess.run(
        [str(tool), "-c", str(source), "-b", "4096", "-p", "256", "-s", str(size), str(target)],
        check=True,
    )
    return offset, target
