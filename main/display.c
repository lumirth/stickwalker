#include "display.h"

#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum {
    RESET = GPIO_NUM_21, CS = GPIO_NUM_41, DC = GPIO_NUM_45,
    SCK = GPIO_NUM_40, DATA = GPIO_NUM_39, BACKLIGHT = GPIO_NUM_38, ROWS = 8,
};

typedef enum {
    PANEL_OFF,
    PANEL_POWERED,
    PANEL_RESET_RELEASED,
    PANEL_SLEEP_OUT,
    PANEL_DRAWING,
    PANEL_LIT,
} PanelState;

static const uint64_t panel_pins = (1ULL << RESET) | (1ULL << CS) | (1ULL << DC) |
                                  (1ULL << SCK) | (1ULL << DATA) | (1ULL << BACKLIGHT);
static spi_device_handle_t spi;
static PanelState state;
static uint64_t due;
static bool inverted;
static DiodeIcon diode;
static const uint16_t diode_icons[4][16] = {
{0x0000, 0x0078, 0x0084, 0x0102, 0x0102, 0x0102, 0x0102, 0x01fe,
     0x0048, 0x0048, 0x4448, 0x2848, 0x1048, 0x2848, 0x4400, 0x0000},
{0x0000, 0x0078, 0x00fc, 0x01f6, 0x01f6, 0x01fe, 0x01fe, 0x01fe,
     0x0048, 0x2048, 0x7c48, 0x2048, 0x0048, 0x0848, 0x7c00, 0x0800},
{0x0000, 0x0078, 0x00fc, 0x01f6, 0x01f6, 0x01fe, 0x01fe, 0x01fe,
     0x0048, 0x0048, 0x0048, 0x0048, 0x0048, 0x0048, 0x0000, 0x0000},
{0x0000, 0x0078, 0x00fc, 0x01f6, 0x01f6, 0x01fe, 0x01fe, 0x11fe,
     0x2048, 0x4448, 0x4848, 0x4848, 0x4848, 0x4448, 0x2000, 0x1000}
};

static bool orientation_pending;
static bool pwm;
static unsigned selected;
static unsigned intensity = 100;
static uint8_t picture[SCREEN_H][SCREEN_W];
static bool rows[SCREEN_H];
DMA_ATTR static uint16_t dma_pixels[ROWS * SCREEN_W];

char palette_names[PALETTES][16] = {"Walker LCD", "Greyscale 2", "Greyscale", "Poketch", "DMG", "Amber"};
#define RGB565(c) ((uint16_t)((((c) >> 8) & 0xf800) | (((c) >> 5) & 0x07e0) | (((c) >> 3) & 0x001f)))
static const uint16_t default_colors[PALETTES][4] = {
    {RGB565(0xb7b8b0), RGB565(0x808173), RGB565(0x666559), RGB565(0x1f1a17)},
    {RGB565(0xeaeaea), RGB565(0xa4a4a4), RGB565(0x5d5d5d), RGB565(0x171717)},
    {0xffff, RGB565(0xaaaaaa), RGB565(0x555555), 0},
    {RGB565(0x70b070), RGB565(0x508050), RGB565(0x385030), RGB565(0x102818)},
    {RGB565(0xc2c263), RGB565(0x5c6b18), RGB565(0x17320b), RGB565(0x021900)},
    {RGB565(0xf7e2af), RGB565(0xd7c092), RGB565(0xaf936a), RGB565(0x764200)},
};
#undef RGB565
static uint16_t colors[PALETTES][4];

void display_palette_table(uint16_t table[PALETTES][4], bool write)
{
    if (write) {
        memcpy(colors, table ? table : default_colors, sizeof(colors));
        memset(rows, 1, sizeof(rows));
    } else memcpy(table, colors, sizeof(colors));
}


static bool transfer(bool data, const void *bytes, size_t length)
{
    spi_transaction_t transaction = {.length = length * 8, .tx_buffer = bytes};
    gpio_set_level(DC, data);
    if (length <= sizeof(transaction.tx_data)) {
        transaction.flags = SPI_TRANS_USE_TXDATA;
        memcpy(transaction.tx_data, bytes, length);
    }
    return spi_device_polling_transmit(spi, &transaction) == ESP_OK;
}

static bool command(uint8_t code, const void *parameters, size_t length)
{
    return transfer(false, &code, 1) && (!length || transfer(true, parameters, length));
}

static bool orient(void)
{
    // LCMCTRL clears XBGR; this panel needs MADCTL's BGR bit in either rotation.
    const uint8_t access = inverted ? 0xa8 : 0x68;
    return command(0x36, &access, 1);
}

static bool configure_panel(void)
{
    // ST7789 voltage and gamma setup used by M5GFX for the StickS3 panel.
    static const struct {
        uint8_t command, length, data[14];
    } setup[] = {
        {0xb7, 1, {0x35}}, {0xbb, 1, {0x28}}, {0xc0, 1, {0x0c}},
        {0xc2, 2, {0x01, 0xff}}, {0xc3, 1, {0x10}}, {0xc4, 1, {0x20}},
        {0xd0, 2, {0xa4, 0xa1}}, {0xb0, 2, {0x00, 0xc0}},
        {0xe0, 14, {0xd0,0x00,0x02,0x07,0x0a,0x28,0x32,0x44,0x42,0x06,0x0e,0x12,0x14,0x17}},
        {0xe1, 14, {0xd0,0x00,0x02,0x07,0x0a,0x28,0x31,0x54,0x47,0x0e,0x1c,0x17,0x1b,0x1e}},
    };
    for (unsigned i = 0; i < sizeof(setup) / sizeof(*setup); ++i) {
        if (!command(setup[i].command, setup[i].data, setup[i].length)) return false;
    }
    return command(0x38, NULL, 0); // Leave idle color mode.
}

static bool write_rows(unsigned y, unsigned count)
{
    unsigned top = y + (inverted ? 52 : 53);
    unsigned bottom = top + count - 1;
    const uint8_t columns[] = {0, 40, 1, 23};
    const uint8_t lines[] = {0, top, 0, bottom};
    for (unsigned i = 0; i < count; ++i) {
        for (unsigned x = 0; x < SCREEN_W; ++x) {
            unsigned shade = picture[y + i][x];
            unsigned ix = inverted ? 220 : 4, iy = inverted ? 115 : 4;
            if (diode && x >= ix && x < ix + 16 && y + i >= iy && y + i < iy + 16) {
                shade = (diode_icons[diode - 1][y + i - iy] >> (15 - (x - ix))) & 1 ? 3 : 0;
            }
            dma_pixels[i * SCREEN_W + x] = __builtin_bswap16(colors[selected][shade]);
        }
    }
    return command(0x2a, columns, sizeof(columns)) && command(0x2b, lines, sizeof(lines)) &&
           command(0x2c, NULL, 0) && transfer(true, dma_pixels, count * SCREEN_W * 2);
}

static void backlight(void)
{
    if (!pwm) {
        ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0,
                                     .duty_resolution = LEDC_TIMER_8_BIT, .freq_hz = 5000,
                                     .clk_cfg = LEDC_USE_XTAL_CLK};
        ledc_channel_config_t channel = {.gpio_num = BACKLIGHT, .speed_mode = LEDC_LOW_SPEED_MODE,
                                         .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0,
                                         .duty = intensity};
        pwm = ledc_timer_config(&timer) == ESP_OK && ledc_channel_config(&channel) == ESP_OK;
    }
    if (pwm) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, intensity);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
}

void display_power(bool on)
{
    if (on) {
        if (state != PANEL_OFF || (uint64_t)esp_timer_get_time() < due) {
            return;
        }
        board_park(panel_pins);
        if (!board_lcd_power(true)) {
            due = esp_timer_get_time() + 500000;
            return;
        }
        state = PANEL_POWERED;
        due = esp_timer_get_time() + 10000;
        return;
    }
    if (state == PANEL_OFF) {
        return;
    }
    if (pwm) {
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        ledc_timer_pause(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0);
        const ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE,
                                           .timer_num = LEDC_TIMER_0, .deconfigure = true};
        ledc_timer_config(&timer);
        pwm = false;
    }
    if (spi) {
        if (state == PANEL_LIT) {
            command(0x28, NULL, 0);
            command(0x10, NULL, 0);
        }
        spi_bus_remove_device(spi);
        spi_bus_free(SPI3_HOST);
        spi = NULL;
    }
    board_park(panel_pins);
    board_lcd_power(false);
    state = PANEL_OFF;
    due = 0;
}

void display_style(unsigned palette, bool reverse, unsigned brightness)
{
    if (selected != palette || inverted != reverse) {
        memset(rows, 1, sizeof(rows));
    }
    orientation_pending |= inverted != reverse;
    selected = palette;
    inverted = reverse;
    if (intensity != brightness) {
        intensity = brightness;
        if (state == PANEL_LIT) {
            backlight();
        }
    }
}

static uint64_t panel_retry(void)
{
    display_power(false);
    due = esp_timer_get_time() + 500000;
    return due;
}

uint64_t display_tick(void)
{
    uint64_t now = esp_timer_get_time();
    if (now < due) {
        return due;
    }
    if (state == PANEL_OFF) {
        return UINT64_MAX;
    }
    if (state == PANEL_POWERED) {
        spi_bus_config_t bus = {
            .mosi_io_num = DATA,
            .miso_io_num = -1,
            .sclk_io_num = SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = sizeof(dma_pixels),
        };
        spi_device_interface_config_t device = {
            .clock_speed_hz = 40000000,
            .spics_io_num = CS,
            .queue_size = 1,
            .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_3WIRE,
        };
        if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
            return panel_retry();
        }
        if (spi_bus_add_device(SPI3_HOST, &device, &spi) != ESP_OK) {
            spi_bus_free(SPI3_HOST);
            return panel_retry();
        }
        gpio_set_level(RESET, 1);
        state = PANEL_RESET_RELEASED;
        due = esp_timer_get_time() + 120000;
        return due;
    }
    if (state == PANEL_RESET_RELEASED) {
        if (!configure_panel() || !command(0x11, NULL, 0)) {
            return panel_retry();
        }
        state = PANEL_SLEEP_OUT;
        due = esp_timer_get_time() + 120000;
        return due;
    }
    if (state == PANEL_SLEEP_OUT) {
        uint8_t format = 0x55;
        if (!command(0x3a, &format, 1) || !orient() ||
            !command(0x21, NULL, 0) || !command(0x13, NULL, 0)) {
            return panel_retry();
        }
        orientation_pending = false;
        memset(rows, 1, sizeof(rows));
        state = PANEL_DRAWING;
    }
    if (orientation_pending) {
        if (!orient()) {
            return panel_retry();
        }
        orientation_pending = false;
    }
    for (unsigned y = 0; y < SCREEN_H;) {
        if (!rows[y]) {
            ++y;
            continue;
        }
        unsigned count = 1;
        while (count < ROWS && y + count < SCREEN_H && rows[y + count]) {
            ++count;
        }
        if (!write_rows(y, count)) {
            return panel_retry();
        }
        memset(rows + y, 0, count * sizeof(rows[0]));
        // Let the game service note deadlines between SPI transfers.
        return now;
    }
    if (state == PANEL_DRAWING) {
        if (!command(0x29, NULL, 0)) {
            return panel_retry();
        }
        state = PANEL_LIT;
        backlight();
    }
    return UINT64_MAX;
}

static void pixel(int x, int y, unsigned shade)
{
    if ((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H && picture[y][x] != shade) {
        picture[y][x] = shade;
        rows[y] = true;
    }
}

void display_rect(int x, int y, int w, int h, unsigned shade)
{
    for (int py = y; py < y + h; ++py) {
        for (int px = x; px < x + w; ++px) {
            pixel(px, py, shade);
        }
    }
}

void display_fill(unsigned shade)
{
    display_rect(0, 0, SCREEN_W, SCREEN_H, shade);
}

void display_game(const uint8_t *pixels)
{
    for (unsigned y = 0; y < 64; ++y) {
        for (unsigned x = 0; x < 96; ++x) {
            unsigned shade = *pixels++ & 3;
            int left = 24 + x * 2, top = 3 + y * 2;
            pixel(left, top, shade);
            pixel(left + 1, top, shade);
            pixel(left, top + 1, shade);
            pixel(left + 1, top + 1, shade);
        }
    }
}

// Five columns per glyph, lowest bit at the top.
static const uint8_t alphabet[][5] = {
    {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36}, {0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c}, {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f}, {0x00,0x41,0x7f,0x41,0x00},
    {0x20,0x40,0x41,0x3f,0x01}, {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f}, {0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06}, {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01}, {0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f}, {0x3f,0x40,0x38,0x40,0x3f}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
};
static const uint8_t digits[][5] = {
    {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4b,0x31}, {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, {0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1e},
};

static const uint8_t lowercase[][5] = {
    {0x20, 0x54, 0x54, 0x54, 0x78}, {0x7f, 0x48, 0x44, 0x44, 0x38},
    {0x38, 0x44, 0x44, 0x44, 0x20}, {0x38, 0x44, 0x44, 0x48, 0x7f},
    {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7e, 0x09, 0x01, 0x02},
    {0x0c, 0x52, 0x52, 0x52, 0x3e}, {0x7f, 0x08, 0x04, 0x04, 0x78},
    {0x00, 0x44, 0x7d, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3d, 0x00},
    {0x7f, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7f, 0x40, 0x00},
    {0x7c, 0x04, 0x18, 0x04, 0x78}, {0x7c, 0x08, 0x04, 0x04, 0x78},
    {0x38, 0x44, 0x44, 0x44, 0x38}, {0x7c, 0x14, 0x14, 0x14, 0x08},
    {0x08, 0x14, 0x14, 0x18, 0x7c}, {0x7c, 0x08, 0x04, 0x04, 0x08},
    {0x48, 0x54, 0x54, 0x54, 0x24}, {0x04, 0x3f, 0x44, 0x40, 0x20},
    {0x3c, 0x40, 0x40, 0x20, 0x7c}, {0x1c, 0x20, 0x40, 0x20, 0x1c},
    {0x3c, 0x40, 0x30, 0x40, 0x3c}, {0x44, 0x28, 0x10, 0x28, 0x44},
    {0x0c, 0x50, 0x50, 0x50, 0x3c}, {0x44, 0x64, 0x54, 0x4c, 0x44},
};

// Remaining printable ASCII; palette names use the same character set as the host.
static const struct { char character; uint8_t columns[5]; } punctuation[] = {
    {'!', {0,0,0x5f,0,0}}, {'"', {0,7,0,7,0}}, {'#', {0x14,0x7f,0x14,0x7f,0x14}},
    {'$', {0x24,0x2a,0x7f,0x2a,0x12}}, {'&', {0x36,0x49,0x55,0x22,0x50}},
    {'\'', {0,5,3,0,0}}, {'(', {0,0x1c,0x22,0x41,0}}, {')', {0,0x41,0x22,0x1c,0}},
    {'*', {0x14,8,0x3e,8,0x14}}, {',', {0,0x50,0x30,0,0}},
    {';', {0,0x56,0x36,0,0}}, {'=', {0x14,0x14,0x14,0x14,0x14}},
    {'?', {2,1,0x51,9,6}}, {'@', {0x32,0x49,0x79,0x41,0x3e}},
    {'[', {0,0x7f,0x41,0x41,0}}, {'\\', {2,4,8,0x10,0x20}},
    {']', {0,0x41,0x41,0x7f,0}}, {'^', {4,2,1,2,4}}, {'_', {0x40,0x40,0x40,0x40,0x40}},
    {'`', {0,1,2,4,0}}, {'{', {0,8,0x36,0x41,0}}, {'|', {0,0,0x7f,0,0}},
    {'}', {0,0x41,0x36,8,0}},
};

void display_text(int x, int y, const char *text, unsigned shade)
{
    static const uint8_t plus[] = {8,8,62,8,8}, dot[] = {0,0,64,0,0};
    static const uint8_t percent[] = {0x63,0x13,0x08,0x64,0x63}, colon[] = {0,0,0x24,0,0};
    static const uint8_t dash[] = {8,8,8,8,8}, tilde[] = {8,4,8,16,8};
    static const uint8_t slash[] = {0x20,0x10,0x08,0x04,0x02};
    static const uint8_t less[] = {8,20,34,0,0}, greater[] = {0,0,34,20,8};
    for (; *text && x <= SCREEN_W - 5; ++text, x += 6) {
        char c = *text;
        const uint8_t *glyph = c >= 'A' && c <= 'Z' ? alphabet[c - 'A'] :
                               c >= 'a' && c <= 'z' ? lowercase[c - 'a'] :
                               c >= '0' && c <= '9' ? digits[c - '0'] :
                               c == '+' ? plus : c == '.' ? dot : c == '%' ? percent :
                               c == ':' ? colon : c == '-' ? dash : c == '~' ? tilde :
                               c == '/' ? slash : c == '<' ? less : c == '>' ? greater : NULL;
        if (!glyph) {
            for (unsigned i = 0; i < sizeof(punctuation) / sizeof(punctuation[0]); ++i) {
                if (punctuation[i].character == c) {
                    glyph = punctuation[i].columns;
                    break;
                }
            }
        }
        if (!glyph) continue;
        for (unsigned col = 0; col < 5; ++col) {
            for (unsigned row = 0; row < 7; ++row) {
                if (glyph[col] & (1u << row)) {
                    pixel(x + col, y + row, shade);
                }
            }
        }
    }
}

void display_diode(DiodeIcon icon)
{
    if (diode == icon) return;
    diode = icon;
    for (unsigned i = 0; i < 16; ++i) rows[4 + i] = rows[115 + i] = true;
}
