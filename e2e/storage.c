#include "../main/save.c"
#include "fake_idf.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static uint8_t flash[0x1f0000];
static uint8_t prepared[sizeof(flash)];
static uint8_t previous[65536];
static uint8_t expected[65536];
static uint8_t next_image[65536];
static esp_partition_t device;
static unsigned programs, erases, reads, cut_program;
static bool cut_half;
static bool fail_program;
static unsigned erase_commands, cut_erase;
static jmp_buf cut;

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label) {
    return &device;
}
int esp_reset_reason(void) {
    return ESP_RST_POWERON;
}
uint32_t esp_rom_crc32_le(uint32_t seed, const void *data, size_t n) {
    return (uint32_t)crc32(seed, data, (uInt)n);
}
int esp_partition_read(const esp_partition_t *part, size_t at, void *out, size_t n) {
    assert(at + n <= sizeof(flash));
    ++reads;
    memcpy(out, flash + at, n);
    return ESP_OK;
}
int esp_partition_erase_range(const esp_partition_t *part, size_t at, size_t n) {
    assert(at % 4096 == 0 && n % 4096 == 0 && at + n <= sizeof(flash));
    ++erases;
    while (n) {
        size_t count = n >= BLOCK && (at + 0x610000) % BLOCK == 0 ? BLOCK : SECTOR;
        ++erase_commands;
        size_t erased = erase_commands == cut_erase && cut_half ? count / 2 : count;
        memset(flash + at, 0xff, erased);
        if (erase_commands == cut_erase)
            longjmp(cut, 1);
        at += count;
        n -= count;
    }
    return ESP_OK;
}
int esp_partition_write(const esp_partition_t *part, size_t at, const void *bytes, size_t n) {
    const uint8_t *source = bytes;
    assert(at + n <= sizeof(flash));
    while (n) {
        size_t count = n < 64 ? n : 64;
        if (count > 256 - at % 256)
            count = 256 - at % 256;
        ++programs;
        size_t written = programs == cut_program && cut_half ? count / 2 : count;
        for (size_t i = 0; i < written; ++i) {
            assert((flash[at + i] & source[i]) == source[i]);
            flash[at + i] &= source[i];
        }
        if (programs == cut_program)
            longjmp(cut, 1);
        if (fail_program) {
            fail_program = false;
            return -1;
        }
        at += count;
        source += count;
        n -= count;
    }
    return ESP_OK;
}

void pw_host_fault(const char *message) {
    fprintf(stderr, "%s\n", message);
    abort();
}

static bool open_store(void) {
    memset(&store, 0, sizeof(store));
    memset(&retained, 0, sizeof(retained));
    memset(eeprom, 0, sizeof(eeprom));
    memset(dirty_ranges, 0, sizeof(dirty_ranges));
    partition = NULL;
    return save_open();
}

static void boot_store(void) {
    assert(open_store());
}

static void upload(void) {
    uint8_t bytes[128];
    assert(save_prepare_transfer());
    unsigned start_reads = reads, start_programs = programs, start_erases = erases;
    assert(save_begin_transfer());
    assert(reads == start_reads && programs == start_programs && erases == start_erases);
    for (unsigned page = 0; page < 87; ++page) {
        for (unsigned i = 0; i < sizeof(bytes); ++i)
            bytes[i] = (uint8_t)(page * 19 + i * 7 + 11);
        pw_host_eeprom_write(0xd480 + page * 128, bytes, sizeof(bytes));
    }
    assert(reads == start_reads && programs == start_programs && erases == start_erases);
}

static void complete(void) {
    uint8_t page[128];
    for (unsigned i = 0; i < 82; ++i) {
        pw_host_eeprom_read(0xd700 + i * 128, page, 128);
        pw_host_eeprom_write(0x8f00 + i * 128, page, 128);
    }
    for (unsigned i = 0; i < 5; ++i) {
        pw_host_eeprom_read(0xd480 + i * 128, page, 128);
        pw_host_eeprom_write(0xcc00 + i * 128, page, 128);
    }
    uint8_t zero = 0;
    for (unsigned at = 0xcf27; at < 0xdbff; at += 132)
        pw_host_eeprom_write(at, &zero, 1);
    uint8_t zeros[128] = {0};
    for (unsigned at = 0xde24; at < 0xf380; at += 128)
        pw_host_eeprom_write(at, zeros, 128);
    uint8_t flags = 3;
    pw_host_eeprom_write(0x148, &flags, 1);
    assert(save_commit());
    save_end_transfer();
}

int main(void) {
    memset(flash, 0xff, sizeof(flash));
    for (unsigned i = 0; i < 65536; ++i)
        flash[i] = (uint8_t)(i * 13 + i / 128);
    memcpy(flash, "nintendo", 8);
    boot_store();
    assert(save_commit()); // Prepare a published bank and erased space.
    assert(!memcmp(flash, "\0\0\0\0", 4));
    memcpy(prepared, flash, sizeof(flash));
    memcpy(previous, eeprom, sizeof(eeprom));
    programs = 0;
    upload();
    complete();
    memcpy(expected, eeprom, sizeof(eeprom));
    unsigned total = programs;
    boot_store();
    assert(memcmp(eeprom, expected, sizeof(eeprom)) == 0);

    for (unsigned half = 0; half < 2; ++half) {
        for (unsigned target = 1; target <= total; ++target) {
            memcpy(flash, prepared, sizeof(flash));
            cut_program = 0;
            boot_store();
            programs = 0;
            cut_program = target;
            cut_half = half;
            if (setjmp(cut) == 0) {
                upload();
                complete();
                assert(!"Expected power cut");
            }
            cut_program = 0;
            boot_store();
            assert(!memcmp(eeprom, previous, sizeof(eeprom)) ||
                   !memcmp(eeprom, expected, sizeof(eeprom)));
            if (store.generation) {
                assert(!memcmp(flash, "\0\0\0\0", 4));
            }
            const uint8_t *wanted = !memcmp(eeprom, expected, sizeof(eeprom)) ? expected : previous;
            memcpy(next_image, wanted, sizeof(next_image));
            next_image[43] ^= 0x5a;
            pw_host_eeprom_write(43, next_image + 43, 1);
            assert(save_commit());
            boot_store();
            assert(memcmp(eeprom, next_image, sizeof(eeprom)) == 0);
        }
    }
    printf("%u hardware-program cut points, both halfway and after programming: old or fully "
           "committed 64 KiB image recovered, then a new save and reboot succeeded\n",
           total);

    memcpy(flash, prepared, sizeof(flash));
    boot_store();
    upload();
    unsigned generation = store.generation;
    boot_store();
    uint8_t changed = previous[42] ^ 0x55;
    pw_host_eeprom_write(42, &changed, 1);
    assert(save_commit());
    assert(store.generation == generation);
    boot_store();
    previous[42] = changed;
    assert(memcmp(eeprom, previous, sizeof(eeprom)) == 0);
    printf("RAM-only unfinished upload left its bank unchanged; next commit survived reboot\n");

    memcpy(flash, prepared, sizeof(flash));
    boot_store();
    memcpy(previous, eeprom, sizeof(eeprom));
    upload();
    unsigned before_programs = programs;
    unsigned before_erases = erases;
    save_end_transfer();
    assert(programs == before_programs && erases == before_erases);
    assert(memcmp(eeprom, previous, sizeof(eeprom)) == 0);
    generation = store.generation;
    save_begin_transfer();
    save_end_transfer();
    assert(store.generation == generation);
    printf("Live abort restored the complete previous image without flash writes and reused the "
           "bank\n");
    memset(flash, 0xff, sizeof(flash));
    memcpy(flash, previous, sizeof(previous));
    memcpy(prepared, flash, sizeof(flash));
    boot_store();
    changed = previous[43] ^ 0x33;
    pw_host_eeprom_write(43, &changed, 1);
    programs = 0;
    assert(save_commit());
    memcpy(expected, eeprom, sizeof(expected));
    total = programs;
    // Interrupt any subset of the final publication marker's programmed bits.
    // The raw import remains usable until all four marker bytes are complete.
    uint32_t pending = ~BANK_MAGIC;
    unsigned marker_cuts = 0;
    for (uint32_t remaining = pending; remaining; remaining = (remaining - 1) & pending) {
        uint32_t torn = BANK_MAGIC | remaining;
        memcpy(flash, previous, sizeof(previous));
        memcpy(flash + BANK0 + offsetof(BankHeader, magic), &torn, sizeof(torn));
        boot_store();
        assert(!store.generation && !memcmp(eeprom, previous, sizeof(previous)));
        ++marker_cuts;
    }
    printf("%u partial publication markers recovered the intact raw import\n", marker_cuts);
    for (unsigned half = 0; half < 2; ++half) {
        for (unsigned target = 1; target <= total; ++target) {
            memcpy(flash, prepared, sizeof(flash));
            cut_program = 0;
            boot_store();
            pw_host_eeprom_write(43, &changed, 1);
            programs = 0;
            cut_program = target;
            cut_half = half;
            if (setjmp(cut) == 0) {
                save_commit();
                assert(!"Expected snapshot power cut");
            }
            cut_program = 0;
            boot_store();
            assert(!memcmp(eeprom, previous, sizeof(eeprom)) ||
                   !memcmp(eeprom, expected, sizeof(eeprom)));
            if (store.generation) {
                assert(!memcmp(flash, "\0\0\0\0", 4));
            }
        }
    }
    printf("%u first-bank program cut points, both halfway and after programming: old or complete "
           "new image recovered\n",
           total);
    // Reuse an older published bank: interrupt both erasure and programming
    // while preparing Connect, then recover and complete a new transfer.
    cut_program = 0;
    cut_erase = 0;
    memset(flash, 0xff, sizeof(flash));
    memcpy(flash, previous, sizeof(previous));
    boot_store();
    assert(save_commit());
    uint8_t latest = eeprom[45] ^ 0x73;
    pw_host_eeprom_write(45, &latest, 1);
    assert(publish_bank(BANK0 + BANK_SIZE));
    memcpy(previous, eeprom, sizeof(previous));
    memcpy(prepared, flash, sizeof(flash));
    store.tail = BANK_SIZE;
    programs = erase_commands = 0;
    assert(save_prepare_transfer());
    assert(save_begin_transfer());
    unsigned prepare_programs = programs, prepare_erases = erase_commands;
    for (unsigned erase_cut = 0; erase_cut < 2; ++erase_cut) {
        unsigned count = erase_cut ? prepare_erases : prepare_programs;
        for (unsigned half = 0; half < 2; ++half) {
            for (unsigned target = 1; target <= count; ++target) {
                memcpy(flash, prepared, sizeof(flash));
                boot_store();
                store.tail = BANK_SIZE;
                programs = erase_commands = 0;
                cut_half = half;
                cut_program = erase_cut ? 0 : target;
                cut_erase = erase_cut ? target : 0;
                if (setjmp(cut) == 0) {
                    assert(save_prepare_transfer());
                    assert(save_begin_transfer());
                    assert(!"Expected preparation cut");
                }
                cut_program = cut_erase = 0;
                boot_store();
                assert(memcmp(eeprom, previous, sizeof(previous)) == 0);
                assert(save_prepare_transfer());
                assert(save_begin_transfer());
                uint8_t change = previous[46] ^ 0x49;
                pw_host_eeprom_write(46, &change, 1);
                assert(save_commit());
                save_end_transfer();
                boot_store();
                memcpy(next_image, previous, sizeof(previous));
                next_image[46] = change;
                assert(memcmp(eeprom, next_image, sizeof(eeprom)) == 0);
            }
        }
    }
    printf("Preparation: %u program + %u erase command cuts, twice each; previous image and next "
           "save recovered\n",
           prepare_programs, prepare_erases);
    memcpy(flash, prepared, sizeof(flash));
    boot_store();
    store.tail = BANK_SIZE;
    assert(save_prepare_transfer());
    assert(save_begin_transfer());
    memcpy(prepared, flash, sizeof(flash));
    uint8_t replacement = previous[47] ^ 0x31;
    pw_host_eeprom_write(47, &replacement, 1);
    programs = 0;
    assert(save_commit());
    memcpy(expected, eeprom, sizeof(expected));
    total = programs;
    for (unsigned half = 0; half < 2; ++half) {
        for (unsigned target = 1; target <= total; ++target) {
            memcpy(flash, prepared, sizeof(flash));
            boot_store();
            store.tail = BANK_SIZE;
            pw_host_eeprom_write(47, &replacement, 1);
            programs = 0;
            cut_program = target;
            cut_half = half;
            if (setjmp(cut) == 0) {
                save_commit();
                assert(!"Expected bank replacement cut");
            }
            cut_program = 0;
            boot_store();
            assert(!memcmp(eeprom, previous, sizeof(eeprom)) ||
                   !memcmp(eeprom, expected, sizeof(eeprom)));
        }
    }
    printf(
        "%u replacement-bank program cut points, twice each: old or complete new image recovered\n",
        total);
    // Arbitrary imported bytes can produce a full-image delta. Replay and the
    // exporter must agree even when the encoded record exceeds 65535 bytes.
    boot_store();
    memcpy(expected, eeprom, sizeof(expected));
    for (unsigned i = 8; i < sizeof(expected); ++i) expected[i] ^= 0x5a;
    assert(pw_host_eeprom_write(0, expected, sizeof(expected)));
    assert(save_commit());
    boot_store();
    assert(!memcmp(eeprom, expected, sizeof(eeprom)));
    FILE *out = fopen("build/storage-flash.bin", "wb");
    assert(out && fwrite(flash, 1, sizeof(flash), out) == sizeof(flash));
    fclose(out);
    out = fopen("build/storage-expected.bin", "wb");
    assert(out && fwrite(eeprom, 1, sizeof(eeprom), out) == sizeof(eeprom));
    fclose(out);
    printf("Full-image delta recovered all 65536 bytes\n");

    // An unsupported store must never resurrect its original raw import.
    for (size_t base = BANK0; base <= BANK0 + BANK_SIZE; base += BANK_SIZE) {
        for (size_t shift = 0; shift <= SECTOR; shift += SECTOR) {
            memset(flash, 0xff, sizeof(flash));
            memcpy(flash, previous, sizeof(previous));
            memcpy(flash + base + shift + offsetof(BankHeader, magic), "SWK3", 4);
            unsigned before_writes = programs + erases;
            assert(!open_store());
            assert(programs + erases == before_writes);
        }
    }
    // A pre-Connect checkpoint can roll banks after idle preparation.
    memset(flash, 0xff, sizeof(flash));
    memcpy(flash, previous, sizeof(previous));
    boot_store();
    assert(save_commit());
    store.tail = BANK_SIZE;
    assert(save_prepare_transfer());
    assert(save_commit());
    unsigned before_connect = reads + programs + erases;
    assert(save_begin_transfer());
    assert(reads + programs + erases == before_connect);
    save_end_transfer();
    printf("Checkpoint rollover retained prepared capacity for Connect without flash I/O\n");
    // A runtime NOR error keeps the live image, then retries into the other bank.
    memset(flash, 0xff, sizeof(flash));
    memcpy(flash, previous, sizeof(previous));
    boot_store();
    assert(save_commit());
    memcpy(next_image, eeprom, sizeof(eeprom));
    next_image[91] ^= 0x35;
    pw_host_eeprom_write(91, next_image + 91, 1);
    fail_program = true;
    assert(!save_commit());
    assert(!memcmp(eeprom, next_image, sizeof(eeprom)));
    assert(save_commit());
    boot_store();
    assert(!memcmp(eeprom, next_image, sizeof(eeprom)));
    printf("Runtime program failure preserved RAM; retry and reboot recovered the new save\n");
    // A consumed import remains ineligible even when both bank headers are lost.
    memset(flash, 0xff, sizeof(flash));
    memcpy(flash, previous, sizeof(previous));
    memset(flash, 0, 4);
    assert(!open_store());
    // The same protection applies to a walker initialized without an import.
    memset(flash, 0xff, sizeof(flash));
    boot_store();
    assert(save_commit());
    assert(!memcmp(flash, "\0\0\0\0", 4));
    unsigned before_writes = programs + erases;
    boot_store();
    assert(programs + erases == before_writes);
    memset(flash + BANK0, 0xff, sizeof(BankHeader));
    assert(!open_store());
    printf("Consumed imports and unsupported banks never fall back to a stale or blank walker\n");
    return 0;
}
