#!/usr/bin/env python3
"""Cut power within each NOR command and check recovery using the real save.c."""
from pathlib import Path
import os
import subprocess
import sys
from tempfile import TemporaryDirectory

root = Path(__file__).resolve().parents[1]
from storage_reader import decode

with TemporaryDirectory(prefix='stick-storage-') as temporary:
    headers = Path(temporary)
    (headers / 'fake_idf.h').write_text('''#pragma once
#include <stddef.h>
#include <stdint.h>
#define EXT_RAM_BSS_ATTR
#define RTC_NOINIT_ATTR
#define ESP_OK 0
#define ESP_RST_POWERON 1
#define ESP_PARTITION_TYPE_DATA 1
typedef struct { int unused; } esp_partition_t;
const esp_partition_t *esp_partition_find_first(int, int, const char *);
int esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
int esp_partition_write(const esp_partition_t *, size_t, const void *, size_t);
int esp_partition_erase_range(const esp_partition_t *, size_t, size_t);
int esp_reset_reason(void);
uint32_t esp_rom_crc32_le(uint32_t, const void *, size_t);
''')
    for name in ('esp_attr.h', 'esp_partition.h', 'esp_rom_crc.h', 'esp_system.h'):
        (headers / name).write_text('#include "fake_idf.h"\n')
    binary = headers / 'storage'
    subprocess.run([os.environ.get('CC', 'cc'), '-O1', '-g', '-fsanitize=address,undefined',
                    '-I', str(headers), '-I', str(root / 'pokewalker/include'),
                    str(root / 'e2e/storage.c'), '-lz', '-o', str(binary)], check=True)
    (root / 'build').mkdir(exist_ok=True)
    subprocess.run([binary], cwd=root, check=True)
    flash = (root / 'build/storage-flash.bin').read_bytes()
    actual, _ = decode(flash)
    assert actual == (root / 'build/storage-expected.bin').read_bytes()
    damaged = bytearray(flash)
    for base in (0x10000, 0xF0000):
        damaged[base + 64] ^= 1
    try:
        decode(damaged)
    except ValueError:
        pass
    else:
        raise AssertionError('Corrupt banks were exported as the original imported save')
    print('Rust diagnostic reader matches firmware recovery and rejects corrupt banks')
    for base in (0x10000, 0x11000, 0xF0000, 0xF1000):
        legacy = bytearray(b'\xff' * len(flash))
        legacy[:8] = b'nintendo'
        legacy[base + 68:base + 72] = b'SWK3'
        try:
            decode(legacy)
        except ValueError:
            pass
        else:
            raise AssertionError('Unsupported bank was exported as its stale import')
    first_save = bytearray(b'\xff' * len(flash))
    first_save[:65536] = (root / 'build/storage-expected.bin').read_bytes()
    for version in range(256):
        if version != ord('6') and version & ord('6') == ord('6'):
            first_save[0x10000 + 68:0x10000 + 72] = b'SWK' + bytes([version])
            assert decode(first_save)[0] == first_save[:65536]
