#include "management.h"
#include "driver/usb_serial_jtag.h"
#include "esp_rom_crc.h"
#include <string.h>

enum { LIMIT = 4108, END = 0xc0, ESC = 0xdb };
static uint8_t rx[LIMIT], tx[LIMIT * 2 + 2];
static size_t used, sent, pending;
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
    uint8_t reply[LIMIT] = {'S', 'W', rx[2], rx[3], rx[4], rx[5]};
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
    sent = 0;
    tx[pending++] = END;
    for (size_t i = 0; i < length + 12; ++i) {
        emit(reply[i]);
    }
    tx[pending++] = END;
}

void management_init(void)
{
    usb_serial_jtag_driver_config_t config = {.tx_buffer_size = 8192, .rx_buffer_size = 8192};
    usb_serial_jtag_driver_install(&config);
}

void management_poll(void)
{
    if (sent < pending) {
        int n = usb_serial_jtag_write_bytes(tx + sent, pending - sent, 0);
        if (n > 0) {
            sent += n;
        }
        if (sent < pending) {
            return;
        }
    }
    // One request per poll; no waiting on the desktop, including during IR.
    for (unsigned budget = 0; budget < LIMIT * 2; ++budget) {
        uint8_t byte;
        if (usb_serial_jtag_read_bytes(&byte, 1, 0) != 1) {
            break;
        }
        if (byte == END) {
            if (!discard && !escaped && used) {
                dispatch();
            }
            used = 0;
            escaped = false;
            discard = false;
            if (sent < pending) {
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
