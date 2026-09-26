#include "sound_bridge.h"

#include "board_hal.h"
#include "peripheral_power.h"

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
int64_t amplifier_hold_end_us = 0;
int64_t shutdown_retry_us = 0;
bool codec_configured = false;
bool codec_image_valid = false;
unsigned codec_generation = 0;
// Snapshot every register modified by Espressif's ES8311 suspend sequence.
// Warm resume restores the actual proven configuration, not guessed defaults.
constexpr uint8_t kSuspendRegisters[] = {
    0x00, 0x01, 0x02, 0x0d, 0x0e, 0x12, 0x14, 0x15, 0x17, 0x32, 0x45};
uint8_t codec_image[sizeof(kSuspendRegisters)] = {};
// Timer W drives the original piezo with a pulse waveform. The library's
// default tone is a sine, which softens very brief score notes on the Stick's
// speaker. Keep the source pitch and note durations, but render each cycle as
// the digital waveform that the original output mode requested.
constexpr uint8_t kPiezoCycle[16] = {
    255, 255, 255, 255, 255, 255, 255, 255,
      0,   0,   0,   0,   0,   0,   0,   0};
// The Stick S3's AW8737A amplifier takes about 40 ms to start after SHDN
// rises. A Pokewalker menu note can be shorter than that startup interval.
// Start its score clock only after the amplifier has had time to settle.
constexpr int64_t kAmplifierStartupUs = 65000;
constexpr int64_t kAmplifierHoldUs = 500000;
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

void anchor_output_period() {
  // The native period write resets Timer W. A newly emitted note/rest must
  // start at the actual output transition, not inherit overdue cycles from
  // foreground drawing. Otherwise one service call can start and erase an
  // entire short note before the asynchronous speaker task consumes it.
  const int64_t now = esp_timer_get_time();
  if (next_period_us < now) next_period_us = now;
}

bool speaker_power(void *, bool on) {
  using P = m5::M5PM1_Class;
  if (!StickBoardPower().setGPIOOutput(P::gpio3, on)) {
#ifdef PW_STICK_BENCH_CONTROL
    ++power_failures;
#endif
    return false;
  }
  if (!on) {
    if (!codec_configured) return true;
    if (!codec_image_valid) {
      for (unsigned i = 0; i < sizeof(kSuspendRegisters); ++i)
        if (!m5::In_I2C.readRegister(0x18, kSuspendRegisters[i],
                                      &codec_image[i], 1, 100000)) return false;
      codec_image_valid = true;
    }
    // ES8311 suspend, after muting its external amplifier and before ending
    // I2S. The microphone is removed when the shared supply's last owner exits.
    static constexpr uint8_t suspend[][2] = {
        {0x32, 0x00}, {0x17, 0x00}, {0x0e, 0xff}, {0x12, 0x02},
        {0x14, 0x00}, {0x0d, 0xfa}, {0x15, 0x00}, {0x02, 0x10},
        {0x00, 0x00}, {0x00, 0x1f}, {0x01, 0x30}, {0x01, 0x00},
        {0x45, 0x00}, {0x0d, 0xfc}, {0x02, 0x00},
    };
    bool ok = true;
    for (const auto &value : suspend)
      ok = m5::In_I2C.writeRegister8(0x18, value[0], value[1], 100000) && ok;
    if (ok) codec_configured = false;
    return ok;
  }
  codec_configured = true;  // Teardown also covers a partial resume failure.
  if (codec_image_valid && codec_generation == StickPeripheralGeneration()) {
    for (unsigned i = 0; i < sizeof(kSuspendRegisters); ++i)
      if (!m5::In_I2C.writeRegister8(0x18, kSuspendRegisters[i],
                                     codec_image[i], 100000)) return false;
  }
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
  codec_generation = StickPeripheralGeneration();
  codec_image_valid = false;
  codec_configured = true;
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
  amplifier_hold_end_us = 0;
  const bool off = speaker_power(nullptr, false);
  codec_on = false;
  if (speaker_ready) M5.Speaker.end();
  speaker_ready = false;
  if (off) {
    shutdown_retry_us = 0;
    StickPeripheralRelease(StickPeripheral::Sound);
  } else shutdown_retry_us = esp_timer_get_time() + 1000000;
}

bool start_output() {
  if (!StickPeripheralAcquire(StickPeripheral::Sound)) {
    StickPeripheralRelease(StickPeripheral::Sound);
    return false;
  }
  if (!speaker_ready) {
#ifdef PW_STICK_BENCH_CONTROL
    ++begin_count;
#endif
    speaker_ready = M5.Speaker.begin();
    if (!speaker_ready) M5.Speaker.end();  // Tear down partial I2S setup too.
#ifdef PW_STICK_BENCH_CONTROL
    begin_failures += !speaker_ready;
#endif
  }
  if (speaker_ready && !codec_on) codec_on = speaker_power(nullptr, true);
  if (!speaker_ready || !codec_on) { stop_output(); return false; }
  shutdown_retry_us = 0;
  return true;
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
  // Quiet the powered codec even when sound is disabled in the native save
  // or no score has played yet. Preserve its reset configuration for resume.
  codec_generation = StickPeripheralGeneration();
  codec_image_valid = false;
  codec_configured = StickPeripheralAcquire(StickPeripheral::Sound);
  stop_output();
}

extern "C" void StickSoundEnable(void) {
#ifndef PW_STICK_BENCH_CONTROL
  if (test_tone_end_us) stop_output();
#endif
  const bool cold_amp = !codec_on;
  start_output();
  amplifier_hold_end_us = 0;
  enabled = true;
  next_period_us = esp_timer_get_time() +
                   (cold_amp && codec_on ? kAmplifierStartupUs : 0);
}

extern "C" void StickSoundDisable(void) {
  enabled = false;
#ifdef PW_STICK_BENCH_CONTROL
  if (test_tone_end_us) return;
#endif
  stop_tone();
  // A short game cue is often followed by another button press. Leave the
  // amplifier ready briefly, then switch it off without a blocking delay.
  amplifier_hold_end_us = codec_on ? esp_timer_get_time() +
                                      kAmplifierHoldUs : 0;
}

extern "C" void StickSoundPeriod(u16 compare, u8 mode) {
#ifdef PW_STICK_BENCH_CONTROL
  finish_tone_measurement();
#endif
  compare_value = compare;
  output_mode = mode;
#ifdef PW_STICK_BENCH_CONTROL
  if (test_tone_end_us) return;
#endif
  if (!enabled || !compare || !output_mode) {
    stop_tone();
    anchor_output_period();
    return;
  }
  // H8 output modes select progressively stronger drive. The M5 speaker's
  // default master level is only 64/255 and made level 2 effectively silent.
  M5.Speaker.setVolume(output_mode == 1 ? 100 :
                       output_mode == 2 ? 170 : 235);
  if (speaker_ready && codec_on) {
    const bool started = M5.Speaker.tone(
        // Timer W is one voice. Automatic channel allocation would leave
        // preceding pitches playing on other channels until the score ends.
        32768.0f / float(compare + 1), UINT32_MAX, 0, true,
        kPiezoCycle, sizeof(kPiezoCycle));
#ifdef PW_STICK_BENCH_CONTROL
    ++tone_count;
    tone_failures += !started;
    if (started) last_tone_us = uint64_t(esp_timer_get_time());
#else
    (void)started;
#endif
  }
  anchor_output_period();
}

extern "C" void StickSoundSilencePeriod(u16 compare) {
  compare_value = compare;
#ifdef PW_STICK_BENCH_CONTROL
  if (test_tone_end_us) return;
#endif
  stop_tone();
  anchor_output_period();
}

extern "C" void StickSoundMute(void) {
#ifdef PW_STICK_BENCH_CONTROL
  if (test_tone_end_us) return;
#endif
  stop_tone();
}

extern "C" void StickSoundQuiesceForIr(void) {
  stop_output();
}

extern "C" int StickSoundIsBusy(void) {
  return enabled || test_tone_end_us || speaker_ready;
}

extern "C" void StickSoundService(void) {
  if (!enabled && shutdown_retry_us &&
      esp_timer_get_time() >= shutdown_retry_us) stop_output();
  if (test_tone_end_us && esp_timer_get_time() >= test_tone_end_us)
    stop_output();
  if (!enabled && amplifier_hold_end_us &&
      esp_timer_get_time() >= amplifier_hold_end_us)
    stop_output();
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
  if (!start_output()) return 0;
  M5.Speaker.setVolume(170);
  const bool started = M5.Speaker.tone(880.0f, 1000);
#ifdef PW_STICK_BENCH_CONTROL
  ++tone_count;
  tone_failures += !started;
#endif
  if (started) test_tone_end_us = esp_timer_get_time() + 1000000;
  else stop_output();
  return started ? 1 : 0;
}

#ifdef PW_STICK_BENCH_CONTROL
extern "C" int StickSoundBenchTone(void) {
  if (!start_output()) return 0;
  M5.Speaker.setVolume(170);
  const bool started = M5.Speaker.tone(880.0f, 8000);
  ++tone_count;
  tone_failures += !started;
  if (started) test_tone_end_us = esp_timer_get_time() + 8000000;
  else stop_output();
  return started ? 1 : 0;
}

extern "C" int StickSoundBenchToneActive(void) {
  return test_tone_end_us != 0 && esp_timer_get_time() < test_tone_end_us;
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
