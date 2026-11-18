#include "save.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "pokewalker_host.h"

enum {
    EEPROM_SIZE = 65536,
    SECTOR = 4096,
    BLOCK = 65536,
    BANK_SIZE = 14 * BLOCK,
    BANK0 = BLOCK,
    LOG_START = SECTOR + EEPROM_SIZE,
    BANK_MAGIC = 0x364b5753,
    RECORD_MAGIC = 0x36525753,
    ZERO_FILL = 0x8000,
    MAX_SPAN = 0x7fff,
    RECORD_BYTES = EEPROM_SIZE + 3 * 4,
};

typedef struct {
    uint32_t generation;
    PwRuntimeState runtime;
    uint32_t crc;
    uint32_t magic;
} BankHeader;

typedef struct {
    uint32_t magic;
    uint32_t bytes;
    PwRuntimeState runtime;
    uint32_t crc;
} Record;

typedef struct {
    uint16_t address;
    uint16_t length; // ZERO_FILL marks a span without a literal payload.
} Span;

typedef struct {
    uint32_t generation;
    uint32_t position;
    PwRuntimeState runtime;
    uint32_t crc;
} Retained;

static struct {
    size_t base;
    size_t tail;
    size_t erased;
    size_t published;
    size_t prepared_bank;
    bool dirty;
    uint32_t generation;
    PwRuntimeState runtime;
    PwRuntimeState saved_runtime;
    bool in_transfer;
    bool failed;
} store;
static const esp_partition_t *partition;
static RTC_NOINIT_ATTR Retained retained;
static uint8_t eeprom[EEPROM_SIZE];
// One bit would suffice; byte flags keep range merging simple. No second
// EEPROM image or full-record scratch buffer is needed in retained memory.
static uint8_t dirty_ranges[EEPROM_SIZE / 128];
static Record record;
static uint8_t block_check[SECTOR];

_Static_assert(sizeof(BankHeader) == 72, "Bank header layout");
_Static_assert(sizeof(Record) == 72, "Record layout");
_Static_assert(BANK0 + 2 * BANK_SIZE <= 0x1f0000, "walker2 partition layout");

static uint32_t crc(uint32_t seed, const void *data, size_t length) {
    return esp_rom_crc32_le(seed, data, length);
}

static bool is_erased(const void *data, size_t length) {
    const uint8_t *bytes = data;
    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] != 0xff) {
            return false;
        }
    }
    return true;
}

static bool read_at(size_t at, void *data, size_t length) {
    return esp_partition_read(partition, at, data, length) == ESP_OK;
}

static bool erase_blocks(size_t at, size_t length) {
    for (size_t block = at; block < at + length; block += BLOCK) {
        for (size_t offset = 0; offset < BLOCK; offset += SECTOR) {
            if (!read_at(block + offset, block_check, SECTOR)) {
                return false;
            }
            if (!is_erased(block_check, SECTOR)) {
                if (esp_partition_erase_range(partition, block, BLOCK) != ESP_OK) {
                    return false;
                }
                break;
            }
        }
    }
    return true;
}

static bool write_at(size_t at, const void *data, size_t length) {
    return esp_partition_write(partition, at, data, length) == ESP_OK;
}

static bool retire_import(void) {
    // Erased bytes would look like a new walker if its banks became unreadable.
    uint32_t signature;
    const uint32_t consumed = 0;
    return read_at(0, &signature, sizeof(signature)) &&
           (signature == consumed || write_at(0, &consumed, sizeof(consumed)));
}

static bool has_bank_header(void) {
    // Earlier saves placed their bank headers one sector later.
    // A torn SWK6 write can have a higher version byte; its import is still valid.
    for (size_t base = BANK0; base <= BANK0 + BANK_SIZE; base += BANK_SIZE) {
        for (size_t offset = 0; offset <= SECTOR; offset += SECTOR) {
            uint8_t magic[4];
            if (!read_at(base + offset + offsetof(BankHeader, magic), magic, sizeof(magic)) ||
                (!memcmp(magic, "SWK", 3) && magic[3] >= '0' && magic[3] <= '6')) {
                return true;
            }
        }
    }
    return false;
}

static bool read_bank(size_t base, BankHeader *header) {
    return read_at(base, header, sizeof(*header)) && header->magic == BANK_MAGIC &&
           read_at(base + SECTOR, eeprom, EEPROM_SIZE) &&
           header->crc == crc(crc(0, header, offsetof(BankHeader, crc)), eeprom, EEPROM_SIZE);
}

static void remember_commit(void) {
    memset(dirty_ranges, 0, sizeof(dirty_ranges));
    store.saved_runtime = store.runtime;
    store.published = store.tail;
    store.dirty = false;
    Retained next = {store.generation, store.published, store.runtime, 0};
    next.crc = crc(0, &next, offsetof(Retained, crc));
    retained = next;
}

static bool publish_bank(size_t base) {
    BankHeader header = {
        .generation = store.generation + 1, .runtime = store.runtime, .magic = BANK_MAGIC};
    header.crc = crc(crc(0, &header, offsetof(BankHeader, crc)), eeprom, EEPROM_SIZE);
    size_t erased = store.prepared_bank == base ? 3 * BLOCK : 2 * BLOCK;
    if (store.prepared_bank != base && !erase_blocks(base, erased)) {
        return false;
    }
    store.prepared_bank = 0;
    for (size_t offset = 0; offset < EEPROM_SIZE; offset += SECTOR) {
        if (!is_erased(eeprom + offset, SECTOR) &&
            !write_at(base + SECTOR + offset, eeprom + offset, SECTOR)) {
            return false;
        }
    }
    if (!write_at(base, &header, offsetof(BankHeader, magic)) ||
        !write_at(base + offsetof(BankHeader, magic), &header.magic, sizeof(header.magic))) {
        return false;
    }
    store.base = base;
    store.generation = header.generation;
    store.tail = LOG_START;
    store.erased = erased;
    remember_commit();
    return retire_import();
}

static bool apply_spans(size_t payload, bool apply) {
    size_t at = 0;
    while (at < record.bytes) {
        Span span;
        if (at + sizeof(span) > record.bytes || !read_at(payload + at, &span, sizeof(span))) return false;
        at += sizeof(span);
        size_t length = span.length & MAX_SPAN;
        bool zero = span.length & ZERO_FILL;
        if (!length || span.address + length > EEPROM_SIZE ||
            (!zero && at + length > record.bytes)) return false;
        if (apply) {
            if (zero) memset(eeprom + span.address, 0, length);
            else if (!read_at(payload + at, eeprom + span.address, length)) return false;
        }
        if (!zero) at += length;
    }
    return true;
}

static bool valid_record(size_t payload) {
    uint32_t sum = crc(0, &record, offsetof(Record, crc));
    for (size_t at = 0; at < record.bytes;) {
        size_t count = record.bytes - at;
        if (count > sizeof(block_check)) count = sizeof(block_check);
        if (!read_at(payload + at, block_check, count)) return false;
        sum = crc(sum, block_check, count);
        at += count;
    }
    return sum == record.crc && apply_spans(payload, false);
}

static bool load_log(void) {
    size_t at = LOG_START;
    store.published = at;
    while (at + sizeof(Record) <= BANK_SIZE) {
        if (!read_at(store.base + at, &record, sizeof(Record))) {
            return false;
        }
        if (is_erased(&record, sizeof(Record))) {
            break;
        }
        if (record.magic != RECORD_MAGIC || record.bytes > RECORD_BYTES ||
            at + sizeof(Record) + record.bytes > BANK_SIZE) {
            at = BANK_SIZE;
            break;
        }
        size_t payload = store.base + at + sizeof(Record);
        if (!valid_record(payload)) {
            at = BANK_SIZE; // A torn record cannot be overwritten without erasing.
            break;
        }
        if (!apply_spans(payload, true)) return false;
        store.runtime = record.runtime;
        at += sizeof(Record) + record.bytes;
        store.published = at;
    }
    memset(dirty_ranges, 0, sizeof(dirty_ranges));
    store.tail = at;
    store.erased = (at + BLOCK - 1) & ~(BLOCK - 1);
    return true;
}

bool save_open(void) {
    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "walker2");
    if (!partition) {
        return false;
    }
    BankHeader a = {0}, b = {0};
    bool valid_b = read_bank(BANK0 + BANK_SIZE, &b);
    bool valid_a = read_bank(BANK0, &a);
    if (valid_a && (!valid_b || a.generation >= b.generation)) {
        store.base = BANK0;
        store.generation = a.generation;
        store.runtime = a.runtime;
    } else if (valid_b) {
        if (!read_bank(BANK0 + BANK_SIZE, &b)) {
            return false;
        }
        store.base = BANK0 + BANK_SIZE;
        store.generation = b.generation;
        store.runtime = b.runtime;
    } else {
        if (has_bank_header() || !read_at(0, eeprom, EEPROM_SIZE) ||
            (memcmp(eeprom, "nintendo", 8) && !is_erased(eeprom, EEPROM_SIZE))) {
            return false;
        }
        store.tail = BANK_SIZE;
        store.erased = BANK_SIZE;
        remember_commit();
        return true;
    }
    if (!load_log()) {
        return false;
    }
    if (esp_reset_reason() != ESP_RST_POWERON && retained.generation == store.generation &&
        retained.position == store.published &&
        retained.crc == crc(0, &retained, offsetof(Retained, crc))) {
        store.runtime = retained.runtime;
    }
    store.saved_runtime = store.runtime;
    return retire_import();
}

static size_t next_bank(void) {
    return store.generation && store.base == BANK0 ? BANK0 + BANK_SIZE : BANK0;
}

static bool prepare(size_t bytes) {
    size_t end = store.tail + bytes + sizeof(Record);
    if (store.generation && store.erased < BANK_SIZE) {
        size_t limit = end < BANK_SIZE ? (end + BLOCK - 1) & ~(BLOCK - 1) : BANK_SIZE;
        if (limit > store.erased &&
            !erase_blocks(store.base + store.erased, limit - store.erased)) {
            return false;
        }
        if (limit > store.erased) {
            store.erased = limit;
        }
    }
    if (store.generation && end <= BANK_SIZE) return true;
    size_t bank = next_bank();
    if (store.prepared_bank != bank) {
        // A checkpoint may publish this bank before Connect. Leave room
        // for its snapshot and a full transfer record after that checkpoint.
        if (!erase_blocks(bank, 3 * BLOCK)) return false;
        store.prepared_bank = bank;
    }
    return true;
}

static bool next_span(size_t *cursor, Span *span) {
    size_t at = *cursor;
    while (at < EEPROM_SIZE && !dirty_ranges[at / 128]) at = (at / 128 + 1) * 128;
    if (at >= EEPROM_SIZE) return false;
    size_t first = at;
    while (at < EEPROM_SIZE && dirty_ranges[at / 128] && at - first < MAX_SPAN) ++at;
    *span = (Span){first, at - first};
    *cursor = at;
    return true;
}

static size_t changed_bytes(void) {
    size_t cursor = 0, bytes = 0;
    Span span;
    while (next_span(&cursor, &span)) bytes += sizeof(span) + span.length;
    return bytes;
}

bool save_prepare_transfer(void) {
    if (store.in_transfer || store.failed) return false;
    // Also leave room for the ordinary checkpoint immediately before Connect.
    return prepare(2 * (sizeof(Record) + RECORD_BYTES));
}

bool save_begin_import(bool recovery) {
    if (recovery) {
        if (!partition) return false;
        BankHeader a = {0}, b = {0};
        bool valid_a = read_bank(BANK0, &a);
        bool valid_b = read_bank(BANK0 + BANK_SIZE, &b);
        memset(&store, 0, sizeof(store));
        memset(&retained, 0, sizeof(retained));
        memset(dirty_ranges, 0, sizeof(dirty_ranges));
        // A readable checkpoint can still contain an unusable game or log.
        // Publish its replacement in the other bank with a newer generation.
        if (valid_a && (!valid_b || a.generation >= b.generation)) {
            store.base = BANK0;
            store.generation = a.generation;
        } else if (valid_b) {
            store.base = BANK0 + BANK_SIZE;
            store.generation = b.generation;
        }
        store.tail = BANK_SIZE;
        store.erased = BANK_SIZE;
    }
    return save_prepare_transfer() && save_begin_transfer();
}

bool save_begin_transfer(void) {
    size_t end = store.tail + RECORD_BYTES + 2 * sizeof(Record);
    bool ready = store.generation && end <= store.erased;
    if (store.failed || (!ready && store.prepared_bank != next_bank())) {
        return false;
    }
    store.in_transfer = true;
    return true;
}

bool save_commit(void) {
    if (store.failed && !store.in_transfer) {
        // A failed program may have partly written the log tail. Preserve the
        // live RAM image and publish its retry in the other bank.
        store.tail = BANK_SIZE;
        store.failed = false;
    }
    size_t bytes = changed_bytes();
    if (store.failed || !prepare(sizeof(Record) + bytes)) {
        store.failed = true;
        return false;
    }
    if (!store.generation || store.tail + sizeof(Record) + bytes + sizeof(Record) > BANK_SIZE) {
        if (!publish_bank(next_bank())) {
            store.failed = true;
            return false;
        }
        return true;
    }
    record = (Record){.magic = RECORD_MAGIC, .bytes = bytes, .runtime = store.runtime};
    size_t cursor = 0;
    Span span;
    record.crc = crc(0, &record, offsetof(Record, crc));
    while (next_span(&cursor, &span)) {
        record.crc = crc(record.crc, &span, sizeof(span));
        record.crc = crc(record.crc, eeprom + span.address, span.length);
    }
    size_t at = store.base + store.tail;
    if (!write_at(at, &record, sizeof(record))) goto failed;
    at += sizeof(record);
    cursor = 0;
    while (next_span(&cursor, &span)) {
        if (!write_at(at, &span, sizeof(span)) ||
            !write_at(at + sizeof(span), eeprom + span.address, span.length)) goto failed;
        at += sizeof(span) + span.length;
    }
    store.tail += sizeof(Record) + bytes;
    remember_commit();
    return true;
failed:
    store.failed = true;
    return false;
}

void save_end_transfer(void) {
    if (!store.in_transfer) {
        return;
    }
    if (store.dirty) {
        BankHeader header;
        bool restored = store.generation ?
            (read_bank(store.base, &header) && load_log()) : read_at(0, eeprom, EEPROM_SIZE);
        if (!restored) pw_host_fault("State rollback failed");
    }
    store.runtime = store.saved_runtime;
    memset(dirty_ranges, 0, sizeof(dirty_ranges));
    if (store.failed) {
        store.tail = BANK_SIZE; // A failed program may have no readable end.
    }
    store.dirty = false;
    store.in_transfer = false;
    store.failed = false;
}

void pw_host_eeprom_read(uint16_t address, void *out, size_t length) {
    uint8_t *destination = out;
    while (length) {
        size_t count = EEPROM_SIZE - address;
        if (count > length) {
            count = length;
        }
        memcpy(destination, eeprom + address, count);
        destination += count;
        address = 0;
        length -= count;
    }
}

bool pw_host_eeprom_write(uint16_t address, const void *bytes, size_t length) {
    const uint8_t *source = bytes;
    bool changed = false;
    while (length) {
        size_t count = EEPROM_SIZE - address;
        if (count > length) {
            count = length;
        }
        if (memcmp(eeprom + address, source, count)) {
            memcpy(eeprom + address, source, count);
            memset(dirty_ranges + address / 128, 1, (address + count - 1) / 128 - address / 128 + 1);
            store.dirty = true;
            changed = true;
        }
        source += count;
        address = 0;
        length -= count;
    }
    return changed;
}

bool pw_host_runtime_load(PwRuntimeState *runtime) {
    if (!store.runtime.present) {
        return false;
    }
    *runtime = store.runtime;
    return true;
}

void pw_host_runtime_checkpoint(const PwRuntimeState *runtime) {
    store.runtime = *runtime;
    if (!store.in_transfer && !store.dirty) {
        store.saved_runtime = *runtime;
        Retained next = {store.generation, store.published, *runtime, 0};
        next.crc = crc(0, &next, offsetof(Retained, crc));
        retained = next;
    }
}
