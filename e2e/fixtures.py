"""Prepare saved expeditions for long-history gameplay and peer journeys."""
from pathlib import Path
import struct
import sys
from zlib import crc32

from storage_reader import decode


def expedition(source, destination, *, daily, hourly, random_state, identity=None, midnight=False, clock_seconds=None):
    flash = bytearray(source.read_bytes())
    eeprom, runtime = decode(flash[0x610000:])
    eeprom, runtime = bytearray(eeprom), bytearray(runtime)
    struct.pack_into('<II', runtime, 8, daily, hourly)
    struct.pack_into('<I', runtime, 20, random_state)
    struct.pack_into('<I', runtime, 24, daily)
    struct.pack_into('<H', runtime, 38, 500)
    if midnight:
        struct.pack_into('<I', runtime, 32, 86340)  # One minute before midnight.
        runtime[48] = 0
    if clock_seconds is not None:
        struct.pack_into('<I', runtime, 32, clock_seconds)
        runtime[48] = 0
    runtime[49] = 1
    save = bytearray(runtime[24:48])
    for at, size in ((0, 4), (4, 4), (8, 4), (12, 2), (14, 2), (16, 2)):
        save[at:at + size] = save[at:at + size][::-1]
    for at in (0x156, 0x256):
        eeprom[at:at + 24] = save
        eeprom[at + 24] = (1 + sum(save)) & 255
    if identity is not None:
        for at in (0x83, 0x183):
            eeprom[at:at + 40] = identity
            eeprom[at + 40] = (1 + sum(identity)) & 255
        for at in (0xed, 0x1ed):
            eeprom[at + 16:at + 56] = identity
            eeprom[at + 104] = (1 + sum(eeprom[at:at + 104])) & 255
        eeprom[0xcc08:0xcc30] = identity
    flash[0x610000:] = b'\xff' * 0x1f0000
    header = struct.pack('<II', 1, 0) + runtime
    header += struct.pack('<I4s', crc32(eeprom, crc32(header)), b'SWK6')
    flash[0x620000:0x620048] = header
    flash[0x621000:0x631000] = eeprom
    destination.write_bytes(flash)
