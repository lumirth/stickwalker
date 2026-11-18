#include "audio.h"

#include "board.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_timer.h"

enum {
    RATE = 48000,
    FRAMES = 96,
    BUFFERS = 4,
    // AW8737A specifies 40 ms typical. Allow settling margin before unmuting.
    AMP_START_MS = 60,
};
static const uint64_t audio_pins = (1ULL << 14) | (1ULL << 15) | (1ULL << 16) |
                                   (1ULL << 17) | (1ULL << 18);
static i2s_chan_handle_t tx;
typedef enum { AUDIO_OFF, AUDIO_WARMING, AUDIO_PLAYING, AUDIO_HOLDING } AudioState;
static volatile AudioState state;
static uint32_t phase;
static volatile uint32_t increment;
static volatile int32_t amplitude;
static uint64_t output_ready;
static uint64_t warm_until;
static uint64_t quiet_since;

static bool IRAM_ATTR fill_audio(i2s_chan_handle_t channel, i2s_event_data_t *event, void *user)
{
    (void)channel;
    (void)user;
    int16_t *samples = event->dma_buf;
    if (state == AUDIO_WARMING && esp_timer_get_time() >= warm_until) {
        state = increment ? AUDIO_PLAYING : AUDIO_HOLDING;
    }
    uint32_t step = increment;
    int32_t level = step && state == AUDIO_PLAYING ? amplitude : 0;
    for (unsigned i = 0; i < event->size / sizeof(*samples); i += 2) {
        samples[i] = samples[i + 1] = phase & 0x80000000 ? -level : level;
        phase += step;
    }
    return false;
}

static bool configure_codec(void)
{
    // Repeat the reset write to wake the ES8311's I2C filter.
    board_codec_write(0x00, 0x1f);
    static const uint8_t settings[][2] = {
        {0x00,0x1f}, {0x44,0x08}, {0x31,0x60},
        {0x01,0x35}, {0x02,0x00}, {0x04,0x10}, {0x05,0x00},
        {0x09,0x0c}, {0x0b,0x00}, {0x0c,0x00}, {0x0e,0x7a}, {0x0d,0x31},
        {0x10,0x1f}, {0x11,0x7f}, {0x12,0x00}, {0x13,0x10},
        {0x32,0xbf}, {0x37,0x08}, {0x00,0x80},
    };
    for (unsigned i = 0; i < sizeof(settings) / sizeof(*settings); ++i) {
        uint8_t reg = settings[i][0], expected = settings[i][1], actual;
        if (!board_codec_write(reg, expected)) return false;
        if (reg == 0x01 || reg == 0x02 || reg == 0x09 || reg == 0x0c || reg == 0x31) {
            if (!board_codec_read(reg, &actual) || actual != expected) return false;
        }
    }
    return true;
}

static bool audio_on(void)
{
    if (!board_audio_ready()) return false;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = BUFFERS;
    channel.dma_frame_num = FRAMES;
    if (i2s_new_channel(&channel, &tx, NULL) != ESP_OK) {
        return false;
    }
    i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = 18, .bclk = 17, .ws = 15, .dout = 14, .din = -1},
    };
    const i2s_event_callbacks_t callbacks = {.on_sent = fill_audio};
    if (i2s_channel_init_std_mode(tx, &standard) != ESP_OK ||
        i2s_channel_register_event_callback(tx, &callbacks, NULL) != ESP_OK ||
        i2s_channel_enable(tx) != ESP_OK) {
        audio_off();
        return false;
    }
    bool configured = false;
    for (unsigned attempt = 0; attempt < 3 && !configured; ++attempt) {
        configured = configure_codec();
    }
    if (!configured || !board_amp_power(true) || !board_codec_write(0x31, 0x00)) {
        audio_off();
        return false;
    }
    warm_until = esp_timer_get_time() + AMP_START_MS * 1000;
    quiet_since = esp_timer_get_time();
    // Start and stop traverse the same DMA queue, so its latency cancels.
    output_ready = warm_until;
    state = AUDIO_WARMING;
    return true;
}

void audio_off(void)
{
    increment = 0;
    state = AUDIO_OFF;
    if (!tx) {
        return;
    }
    board_codec_write(0x31, 0x60);
    board_amp_power(false);
    i2s_channel_disable(tx);
    i2s_del_channel(tx);
    tx = NULL;
    board_park(audio_pins);
}

bool audio_active(void)
{
    return tx != NULL;
}

void audio_prepare(void)
{
    if (!tx) {
        audio_on();
    }
}

uint64_t audio_tone(uint32_t millihertz, uint8_t volume)
{
    uint64_t now = esp_timer_get_time();
    if (!millihertz || !volume) {
        increment = 0;
        quiet_since = now;
        if (state == AUDIO_PLAYING) state = AUDIO_HOLDING;
        return now;
    }
    if (!tx) {
        if (!audio_on()) {
            return UINT64_MAX;
        }
    }
    Battery battery = board_battery();
    bool limited = battery.known && !battery.external && battery.low;
    amplitude = volume == 1 || limited ? 1600 : 3200;
    increment = ((uint64_t)millihertz << 32) / (RATE * 1000u);
    if (state == AUDIO_HOLDING) state = AUDIO_PLAYING;
    return now > output_ready ? now : output_ready;
}

uint64_t audio_tick(bool score_active)
{
    uint64_t now = esp_timer_get_time();
    if (!tx || increment || score_active) {
        return UINT64_MAX;
    }
    uint64_t off_at = (quiet_since > warm_until ? quiet_since : warm_until) + 50000;
    if (now < off_at) {
        return off_at;
    }
    audio_off();
    return UINT64_MAX;
}
