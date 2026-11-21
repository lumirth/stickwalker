#include "audio.h"

#include "board.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <string.h>

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
static uint32_t phase, sounding, edge;
static volatile uint32_t increment, duty;
static volatile int32_t amplitude;
static int32_t level;
static uint32_t silent = RATE;
static volatile SoundMode mode = SOUND_PIEZO;
static int32_t history[4][4];
// A short two-note chime previews a new sound mode: increment, duty, samples.
static volatile uint32_t preview[2][3];
static volatile unsigned preview_note = 2;
static uint32_t preview_left;

// Biquads in Q29, normalized to a0 = 1: b0, b1, b2, a1, a2. The piezo chain
// is a 3.6 kHz fourth-order high-pass, a -8.5 dB dip at 4.5 kHz for the
// StickS3 speaker cavity and a +7 dB peak at 6.2 kHz for the piezo, fitted
// to a recording of a Pokéwalker against a StickS3. Speaker mode keeps the
// speaker's own tone above 800 Hz.
static const int32_t piezo_filter[4][5] = {
    {357617758, -715235516, 357617758, -674010907, 219589214},
    {432478782, -864957563, 432478782, -815103303, 377940912},
    {474975483, -727877409, 400435234, -727877409, 338539805},
    {595652981, -673784968, 383181172, -673784968, 441963241},
};
static const int32_t speaker_filter[1][5] = {
    {498550765, -997101531, 498550765, -994362916, 462969233},
};
// Q15 output gains: the piezo chain matches the Pokéwalker at -3 dB, and
// Piezo+ is 6 dB louder.
static const int32_t mode_gain[SOUND_MODES] = {23198, 46396, 32768};
// Fade each note in and out over 1.5 ms so the amplifier doesn't thump.
enum { RAMP_SAMPLES = RATE * 3 / 2000 };
static uint64_t warm_until;
static uint64_t quiet_since;

// PolyBLEP residual for a unit step at phase 0, in Q15. It removes the
// harmonics a sampled square wave would fold back into the audible band.
static inline int32_t IRAM_ATTR blep(uint32_t t, uint32_t dt)
{
    if (t < dt) {
        int32_t x = (int32_t)(((uint64_t)t << 15) / dt);
        return -(((32768 - x) * (32768 - x)) >> 15);
    }
    uint32_t u = (uint32_t)0 - t;
    if (u <= dt) {
        int32_t y = (int32_t)(((uint64_t)u << 15) / dt);
        return ((32768 - y) * (32768 - y)) >> 15;
    }
    return 0;
}

static inline int32_t IRAM_ATTR filter(const int32_t (*stages)[5], unsigned count, int32_t x)
{
    for (unsigned i = 0; i < count; ++i) {
        int32_t *h = history[i];
        int64_t acc = (int64_t)stages[i][0] * x + (int64_t)stages[i][1] * h[0] + (int64_t)stages[i][2] * h[1] -
                      (int64_t)stages[i][3] * h[2] - (int64_t)stages[i][4] * h[3];
        int32_t y = (int32_t)(acc >> 29);
        h[1] = h[0];
        h[0] = x;
        h[3] = h[2];
        h[2] = y;
        x = y;
    }
    return x;
}

static bool IRAM_ATTR fill_audio(i2s_chan_handle_t channel, i2s_event_data_t *event, void *user)
{
    (void)channel;
    (void)user;
    int16_t *samples = event->dma_buf;
    if (state == AUDIO_WARMING && esp_timer_get_time() >= warm_until) {
        state = increment || preview_note < 2 ? AUDIO_PLAYING : AUDIO_HOLDING;
    }
    SoundMode current = mode;
    for (unsigned i = 0; i < event->size / sizeof(*samples); i += 2) {
        uint32_t step = increment, width = duty;
        int32_t target = amplitude;
        if (preview_note < 2 && state == AUDIO_PLAYING) {
            if (!preview_left) preview_left = preview[preview_note][2];
            step = preview[preview_note][0];
            width = preview[preview_note][1];
            if (!--preview_left) ++preview_note;
        }
        if (!step || state != AUDIO_PLAYING) target = 0;
        if (step) {
            sounding = step;
            edge = width;
        }
        int32_t ramp = amplitude / RAMP_SAMPLES + 1;
        level = level < target ? (level + ramp < target ? level + ramp : target)
                               : (level - ramp > target ? level - ramp : target);
        int32_t value = 0;
        if (level || sounding) {
            int32_t square = phase < edge ? 32767 : -32767;
            square += blep(phase, sounding) - blep(phase - edge, sounding);
            value = (square * level) >> 15;
            phase += level ? sounding : 0;
        }
        // After a note fades, let the filters ring out for 1 ms, then hold true
        // silence; integer feedback would otherwise settle at -1, not 0.
        silent = level ? 0 : silent + (silent < RATE / 1000u);
        if (silent >= RATE / 1000u) {
            memset(history, 0, sizeof(history));
            samples[i] = samples[i + 1] = 0;
            continue;
        }
        value = current == SOUND_SPEAKER ? filter(speaker_filter, 1, value) : filter(piezo_filter, 4, value);
        value = (value * mode_gain[current]) >> 15;
        value = value > 32767 ? 32767 : value < -32767 ? -32767 : value;
        samples[i] = samples[i + 1] = (int16_t)value;
    }
    return false;
}

static uint32_t note_duty(uint32_t millihertz)
{
    // The Pokéwalker's H8 timer divides 32768 Hz by N and toggles at N / 2, so
    // odd dividers are slightly asymmetric and carry even harmonics.
    uint32_t divider = (32768000u + millihertz / 2u) / millihertz;
    return divider ? (uint32_t)(((uint64_t)(divider / 2u) << 32) / divider) : 0x80000000u;
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
        i2s_channel_register_event_callback(tx, &callbacks, NULL) != ESP_OK) {
        audio_off();
        return false;
    }
    bool configured = false;
    for (unsigned attempt = 0; attempt < 3 && !configured; ++attempt) {
        configured = configure_codec();
    }
    if (!configured || !board_amp_power(true)) {
        audio_off();
        return false;
    }
    quiet_since = esp_timer_get_time();
    warm_until = quiet_since + AMP_START_MS * 1000;
    state = AUDIO_WARMING;
    if (i2s_channel_enable(tx) != ESP_OK || !board_codec_write(0x31, 0x00)) {
        audio_off();
        return false;
    }
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
    duty = note_duty(millihertz);
    increment = ((uint64_t)millihertz << 32) / (RATE * 1000u);
    if (state == AUDIO_HOLDING) state = AUDIO_PLAYING;
    return now > warm_until ? now : warm_until;
}

void audio_mode(SoundMode next)
{
    if (next < SOUND_MODES) mode = next;
}

void audio_preview(void)
{
    static const uint32_t notes[2][2] = {{32768000u / 24u, 70}, {32768000u / 27u, 110}};
    if (!tx && !audio_on()) return;
    Battery battery = board_battery();
    amplitude = battery.known && !battery.external && battery.low ? 1600 : 3200;
    for (unsigned i = 0; i < 2; ++i) {
        preview[i][0] = ((uint64_t)notes[i][0] << 32) / (RATE * 1000u);
        preview[i][1] = note_duty(notes[i][0]);
        preview[i][2] = RATE / 1000u * notes[i][1];
    }
    preview_left = 0;
    preview_note = 0;
    quiet_since = esp_timer_get_time() + 180000;
    if (state == AUDIO_HOLDING) state = AUDIO_PLAYING;
}

uint64_t audio_tick(bool score_active)
{
    uint64_t now = esp_timer_get_time();
    if (!tx || increment || score_active || preview_note < 2) {
        return UINT64_MAX;
    }
    uint64_t off_at = (quiet_since > warm_until ? quiet_since : warm_until) + 50000;
    if (now < off_at) {
        return off_at;
    }
    audio_off();
    return UINT64_MAX;
}
