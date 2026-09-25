#include "sound_bridge.h"

#include "board_hal.h"

#include <M5Unified.h>
#include <esp_timer.h>

extern "C" void BeepAdvance(void);

namespace {
bool enabled = false;
bool speaker_ready = false;
bool codec_on = false;
u16 compare_value = 0;
u8 output_mode = 0;
int64_t next_period_us = 0;
int64_t test_tone_end_us = 0;
// Timer W drives the original piezo with a pulse waveform. The library's
// default tone is a sine, which softens very brief score notes on the Stick's
// speaker. Keep the source pitch and note durations, but render each cycle as
// the digital waveform that the original output mode requested.
constexpr uint8_t kPiezoCycle[16] = {
    255, 255, 255, 255, 255, 255, 255, 255,
      0,   0,   0,   0,   0,   0,   0,   0};
#ifdef PW_STICK_BENCH_CONTROL
unsigned begin_count = 0, begin_failures = 0, power_failures = 0;
unsigned tone_count = 0, tone_failures = 0;
uint64_t last_tone_us = 0, shortest_tone_us = UINT64_MAX;
unsigned short_tones_20ms = 0, short_tones_50ms = 0, measured_tones = 0;

void finish_tone_measurement() {
  if (!last_tone_us) return;
  const uint64_t duration = uint64_t(esp_timer_get_time()) - last_tone_us;
  if (duration < shortest_tone_us) shortest_tone_us = duration;
  short_tones_20ms += duration < 20000;
  short_tones_50ms += duration < 50000;
  ++measured_tones;
  last_tone_us = 0;
}
#endif

int64_t period_us() {
  return (int64_t(compare_value + 1) * 1000000 + 32767) / 32768;
}

bool speaker_power(void *, bool on) {
  using P = m5::M5PM1_Class;
  if (!StickBoardPower().setGPIOOutput(P::gpio3, on)) {
#ifdef PW_STICK_BENCH_CONTROL
    ++power_failures;
#endif
    return false;
  }
  if (!on) return true;
  static constexpr uint8_t codec[][2] = {
      {0x00, 0x80}, {0x01, 0xb5}, {0x02, 0x18}, {0x0d, 0x01},
      {0x12, 0x00}, {0x13, 0x10}, {0x32, 0xbf}, {0x37, 0x08},
  };
  for (const auto &register_value : codec)
    if (!m5::In_I2C.writeRegister8(0x18, register_value[0],
                                   register_value[1], 100000)) {
#ifdef PW_STICK_BENCH_CONTROL
      ++power_failures;
#endif
      return false;
    }
  return true;
}

void stop_tone() {
#ifdef PW_STICK_BENCH_CONTROL
  finish_tone_measurement();
#endif
  M5.Speaker.stop();
}

void stop_output() {
  stop_tone();
  test_tone_end_us = 0;
  if (codec_on) {
    speaker_power(nullptr, false);
    codec_on = false;
  }
}
}  // namespace

extern "C" void StickSoundInit(void) {
  // M5.begin() is intentionally absent: it perturbs the GPIO5 receiver.
  // Configure only the Stick S3's ES8311 output and its PM1 power gate.
  auto config = M5.Speaker.config();
  config.pin_mck = 18;
  config.pin_bck = 17;
  config.pin_ws = 15;
  config.pin_data_out = 14;
  config.i2s_port = I2S_NUM_0;
  config.magnification = 1;
  config.sample_rate = 22050;
  // The library defaults to eight 256-frame DMA buffers, enough to queue
  // almost 93 ms of sound at this rate. A short source note may end before
  // its samples are heard. Keep the output queue near 12 ms instead.
  config.dma_buf_len = 64;
  config.dma_buf_count = 4;
  config.task_pinned_core = 0;
  config.stereo = true;
  config.buzzer = false;
  config.use_dac = false;
  config.dac_zero_level = 0;
  M5.Speaker.config(config);
  // Start I2S only when the first real score reaches the output. An idle I2S
  // task on the optical sampler core changes its physical timing.
  speaker_ready = false;
  enabled = false;
  compare_value = 0;
  output_mode = 0;
  stop_output();
}

extern "C" void StickSoundEnable(void) {
  if (test_tone_end_us) stop_output();
  enabled = true;
  next_period_us = esp_timer_get_time();
}

extern "C" void StickSoundDisable(void) {
  enabled = false;
  stop_output();
}

extern "C" void StickSoundPeriod(u16 compare, u8 mode) {
#ifdef PW_STICK_BENCH_CONTROL
  finish_tone_measurement();
#endif
  compare_value = compare;
  output_mode = mode;
  if (!enabled || !compare || !output_mode) {
    stop_tone();
    return;
  }
  // H8 output modes select progressively stronger drive. The M5 speaker's
  // default master level is only 64/255 and made level 2 effectively silent.
  M5.Speaker.setVolume(output_mode == 1 ? 100 :
                       output_mode == 2 ? 170 : 235);
  if (speaker_ready) {
    if (!codec_on) codec_on = speaker_power(nullptr, true);
    if (codec_on) {
      const bool started = M5.Speaker.tone(
          32768.0f / float(compare + 1), UINT32_MAX, -1, true,
          kPiezoCycle, sizeof(kPiezoCycle));
#ifdef PW_STICK_BENCH_CONTROL
      ++tone_count;
      tone_failures += !started;
      if (started) last_tone_us = uint64_t(esp_timer_get_time());
#endif
    }
  } else {
#ifdef PW_STICK_BENCH_CONTROL
    ++begin_count;
#endif
    speaker_ready = M5.Speaker.begin();
#ifdef PW_STICK_BENCH_CONTROL
    begin_failures += !speaker_ready;
#endif
    if (speaker_ready) {
      codec_on = speaker_power(nullptr, true);
      if (codec_on) {
        const bool started = M5.Speaker.tone(
            32768.0f / float(compare + 1), UINT32_MAX, -1, true,
            kPiezoCycle, sizeof(kPiezoCycle));
#ifdef PW_STICK_BENCH_CONTROL
        ++tone_count;
        tone_failures += !started;
        if (started) last_tone_us = uint64_t(esp_timer_get_time());
#endif
      }
    }
  }
}

extern "C" void StickSoundSilencePeriod(u16 compare) {
  compare_value = compare;
  stop_tone();
}

extern "C" void StickSoundMute(void) { stop_tone(); }

extern "C" void StickSoundQuiesceForIr(void) {
  stop_output();
  if (speaker_ready) {
    M5.Speaker.end();
    speaker_ready = false;
  }
}

extern "C" void StickSoundService(void) {
  if (test_tone_end_us && esp_timer_get_time() >= test_tone_end_us)
    stop_output();
#ifdef PW_STICK_BENCH_CONTROL
  // Keep the source sequencer from replacing a diagnostic continuous tone.
  if (test_tone_end_us) return;
#endif
  if (!enabled) return;
  const int64_t now = esp_timer_get_time();
  unsigned serviced = 0;
  while (next_period_us <= now && serviced < 256) {
    BeepAdvance();
    next_period_us += period_us();
    ++serviced;
  }
  if (serviced == 256 && next_period_us <= now)
    next_period_us = now + period_us();
}

extern "C" int StickSoundTestTone(void) {
  if (enabled) return 0;
  if (!speaker_ready) {
#ifdef PW_STICK_BENCH_CONTROL
    ++begin_count;
#endif
    speaker_ready = M5.Speaker.begin();
#ifdef PW_STICK_BENCH_CONTROL
    begin_failures += !speaker_ready;
#endif
  }
  if (!speaker_ready) return 0;
  if (!codec_on) codec_on = speaker_power(nullptr, true);
  if (!codec_on) return 0;
  M5.Speaker.setVolume(170);
  const bool started = M5.Speaker.tone(880.0f, 1000);
#ifdef PW_STICK_BENCH_CONTROL
  ++tone_count;
  tone_failures += !started;
#endif
  if (started) test_tone_end_us = esp_timer_get_time() + 1000000;
  return started ? 1 : 0;
}

#ifdef PW_STICK_BENCH_CONTROL
extern "C" int StickSoundBenchTone(void) {
  if (!speaker_ready) {
    ++begin_count;
    speaker_ready = M5.Speaker.begin();
    begin_failures += !speaker_ready;
  }
  if (!speaker_ready) return 0;
  if (!codec_on) codec_on = speaker_power(nullptr, true);
  if (!codec_on) return 0;
  M5.Speaker.setVolume(170);
  const bool started = M5.Speaker.tone(880.0f, 8000);
  ++tone_count;
  tone_failures += !started;
  if (started) test_tone_end_us = esp_timer_get_time() + 8000000;
  return started ? 1 : 0;
}

extern "C" void StickSoundDiagnostic(unsigned *active, unsigned *ready,
                                       unsigned *codec, unsigned *mode,
                                       unsigned *compare, unsigned *begins,
                                       unsigned *begin_errors,
                                       unsigned *power_errors,
                                       unsigned *tones,
                                       unsigned *tone_errors,
                                       unsigned *playing) {
  *active = enabled;
  *ready = speaker_ready;
  *codec = codec_on;
  *mode = output_mode;
  *compare = compare_value;
  *begins = begin_count;
  *begin_errors = begin_failures;
  *power_errors = power_failures;
  *tones = tone_count;
  *tone_errors = tone_failures;
  *playing = M5.Speaker.isPlaying();
}
extern "C" void StickSoundTimingDiagnostic(unsigned *measured,
                                             unsigned *under_20ms,
                                             unsigned *under_50ms,
                                             unsigned *shortest_us) {
  *measured = measured_tones;
  *under_20ms = short_tones_20ms;
  *under_50ms = short_tones_50ms;
  *shortest_us = shortest_tone_us == UINT64_MAX ? 0 :
                 unsigned(shortest_tone_us);
}
#endif
