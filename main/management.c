#include "management.h"
#include "driver/usb_serial_jtag.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include <string.h>

enum { LIMIT = 4108, END = 0xc0, ESC = 0xdb };
static uint8_t rx[LIMIT], tx[LIMIT * 2 + 2];
static size_t used, pending;
static int64_t tx_deadline, rx_deadline, retry_at;
static bool ready;
static bool escaped, discard;

static uint32_t u32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void emit(uint8_t byte)
{
    if (byte == END || byte == ESC) {
        tx[pending++] = ESC;
        tx[pending++] = byte == END ? 0xdc : 0xdd;
    } else {
        tx[pending++] = byte;
    }
}

static void dispatch(void)
{
    if (used < 12 || memcmp(rx, "SW\x01", 3)) {
        return;
    }
    size_t count = rx[6] | (size_t)rx[7] << 8;
    if (count > 4096 || used != count + 12 ||
        esp_rom_crc32_le(0, rx, used - 4) != u32(rx + used - 4)) {
        return;
    }
    static uint8_t reply[LIMIT];
    memcpy(reply, rx, 6);
    size_t length = 0;
    reply[8] = management_request(rx[3], rx + 8, count, reply + 9, &length);
    if (length > 4095) {
        reply[8] = MANAGE_FAILED;
        length = 0;
    }
    ++length;
    reply[6] = length;
    reply[7] = length >> 8;
    uint32_t crc = esp_rom_crc32_le(0, reply, length + 8);
    for (unsigned i = 0; i < 4; ++i) {
        reply[length + 8 + i] = crc >> (i * 8);
    }
    pending = 0;
    tx[pending++] = END;
    for (size_t i = 0; i < length + 12; ++i) {
        emit(reply[i]);
    }
    tx[pending++] = END;
    tx_deadline = esp_timer_get_time() + 2000000;
}

void management_init(void)
{
    usb_serial_jtag_driver_config_t config = {.tx_buffer_size = 16384, .rx_buffer_size = 8192};
    ready = usb_serial_jtag_driver_install(&config) == ESP_OK;
    retry_at = esp_timer_get_time() + 2000000;
}

// IDF enqueues a whole byte-buffer item or returns zero. A new leading END
// resynchronizes a reader after a dropped reply. SOF alone can flicker at flash writes.
static bool flush_reply(void)
{
    if (pending && usb_serial_jtag_write_bytes(tx, pending, 0) == (int)pending) {
        pending = 0;
    }
    if (!tx_deadline || esp_timer_get_time() < tx_deadline) {
        return !pending;
    }
    if (!pending && usb_serial_jtag_wait_tx_done(0) == ESP_OK) {
        tx_deadline = 0;
        return true;
    }
    // Discard a stalled reply whether it is still here or queued inside IDF.
    usb_serial_jtag_driver_uninstall();
    pending = 0;
    used = 0;
    tx_deadline = 0;
    escaped = false;
    discard = true;
    management_init();
    return false;
}

void management_poll(void)
{
    int64_t now = esp_timer_get_time();
    if (!ready) {
        if (now >= retry_at) management_init();
        return;
    }
    if (!flush_reply()) return;
    if ((used || escaped || discard) && now >= rx_deadline) {
        used = 0;
        escaped = false;
        discard = true;
    }
    // One request per poll; no waiting on the desktop, including during IR.
    for (unsigned budget = 0; budget < LIMIT * 2; ++budget) {
        uint8_t byte;
        if (usb_serial_jtag_read_bytes(&byte, 1, 0) != 1) {
            break;
        }
        rx_deadline = now + 2000000;
        if (byte == END) {
            if (!discard && !escaped && used) {
                dispatch();
            }
            used = 0;
            escaped = false;
            discard = false;
            if (pending) {
                flush_reply();
                break;
            }
        } else if (!discard) {
            if (escaped) {
                if (byte != 0xdc && byte != 0xdd) {
                    discard = true;
                    continue;
                }
                byte = byte == 0xdc ? END : ESC;
                escaped = false;
            } else if (byte == ESC) {
                escaped = true;
                continue;
            }
            if (used == LIMIT) {
                discard = true;
            } else {
                rx[used++] = byte;
            }
        }
    }
}
