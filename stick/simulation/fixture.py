"""Public synthetic EEPROM: native record layouts, original raster labels and score.

Addresses and checksums follow include/eeprom_map.h, resources.h, records.h,
save.h, score.h and the matching firmware's EEPROM/score loaders. No save or
uploaded game artwork is imported. Every fixture uses the current flash-record format.
"""

import struct
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


def course() -> bytes:
    data = bytearray(10430)
    struct.pack_into("<H", data, 0, 25)
    data[12] = 5
    for index in range(190, len(data)):
        data[index] = (index * 37 + 0x5A) & 255
    return bytes(data)


def eeprom(watts: int = 100, companion: bool = False) -> bytes:
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
    status[91] = 3 if companion else 1
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
    if companion:
        image[0xCC08:0xCC30] = identity
        image[0x8F00 : 0x8F00 + 10430] = course()
    return bytes(image)


def storage(partitions: Path, folder: Path, case: str = "registered") -> tuple[int, Path]:
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
        raise ValueError("Firmware has no EEPROM data partition")
    offset, size = partition
    payload = eeprom(
        100 if case in ("registered", "peer-play") else 200, companion=case == "peer-play"
    )

    def record(magic: int, generation: int, sequence: int, data: bytes) -> bytes:
        header = struct.pack("<5I", magic, generation, sequence, len(data), zlib.crc32(data))
        return header + struct.pack("<II", zlib.crc32(header), 0x54494D43) + data

    raw = bytearray(b"\xff" * size)
    checkpoint = record(0x53574531, 2, 2, payload)
    raw[: len(checkpoint)] = checkpoint
    if case not in ("registered", "peer-play"):
        older = eeprom(100)
        patches = b"".join(
            struct.pack("<HH", address, 25) + older[address : address + 25]
            for address in (0x156, 0x256)
        )
        if case == "recovery-journal":
            append = record(0x53574A31, 2, 3, patches)
        elif case == "recovery-stale":
            append = record(0x53574A31, 1, 1, patches)
        elif case == "recovery-truncated":
            append = bytearray(record(0x53574A31, 2, 3, patches)[:-3])
            append[24:28] = b"\xff" * 4
        else:
            raise ValueError(f"Unknown fixture {case}")
        raw[0x22000 : 0x22000 + len(append)] = append
    (folder / "fixture.eeprom").write_bytes(payload)
    target = folder / "fixture.store"
    target.write_bytes(raw)
    return offset, target
