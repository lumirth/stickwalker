#ifdef PW_STICK_BENCH_CONTROL
// Local self-test only. Keep aggregate energy; never retain microphone audio.
#include "foreground_bridge.h"
#include "sound_bridge.h"
#include "board_hal.h"
#include <Arduino.h>
#include <M5Unified.h>
#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <soc/gpio_periph.h>
#include <soc/gpio_sig_map.h>
#include <soc/io_mux_reg.h>
#include <cmath>
#include <cstring>

namespace {
// Diagnostic spectrum only: samples live in bounded RAM and are cleared below.
unsigned peak_bin(const int16_t *samples, unsigned count) {
  if (count < 512) return 0;
  static float real[512], imaginary[512];
  float mean = 0;
  for (unsigned i = 0; i < 512; ++i) mean += samples[i];
  mean /= 512;
  for (unsigned i = 0; i < 512; ++i) {
    unsigned reversed = 0;
    for (unsigned bit = 0; bit < 9; ++bit) reversed = (reversed << 1) | ((i >> bit) & 1);
    real[reversed] = (samples[i] - mean) * (0.5f - 0.5f * std::cos(2 * 3.141592653589793f * i / 511));
    imaginary[reversed] = 0;
  }
  for (unsigned width = 2; width <= 512; width *= 2) {
    const float rotation_real = std::cos(-2 * 3.141592653589793f / width);
    const float rotation_imaginary = std::sin(-2 * 3.141592653589793f / width);
    for (unsigned base = 0; base < 512; base += width) {
      float wr = 1, wi = 0;
      for (unsigned j = 0; j < width / 2; ++j) {
        const unsigned a = base + j, b = a + width / 2;
        const float tr = wr * real[b] - wi * imaginary[b];
        const float ti = wr * imaginary[b] + wi * real[b];
        real[b] = real[a] - tr; imaginary[b] = imaginary[a] - ti;
        real[a] += tr; imaginary[a] += ti;
        const float next = wr * rotation_real - wi * rotation_imaginary;
        wi = wr * rotation_imaginary + wi * rotation_real;
        wr = next;
      }
    }
  }
  unsigned peak = 5;
  float maximum = 0;
  for (unsigned i = 5; i < 120; ++i) {
    const float energy = real[i] * real[i] + imaginary[i] * imaginary[i];
    if (energy > maximum) { maximum = energy; peak = i; }
  }
  std::memset(real, 0, sizeof(real));
  std::memset(imaginary, 0, sizeof(imaginary));
  return peak;
}
}

extern "C" void StickSoundBenchCapture(bool microphone, bool calibration, bool muted) {
  i2s_chan_handle_t rx = nullptr;
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
  channel.dma_desc_num = 4;
  channel.dma_frame_num = 128;
  esp_err_t result = i2s_new_channel(&channel, nullptr, &rx);
  i2s_std_config_t config = {};
  config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(22050);
  config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  config.gpio_cfg.bclk = I2S_GPIO_UNUSED;
  config.gpio_cfg.ws = I2S_GPIO_UNUSED;
  config.gpio_cfg.dout = I2S_GPIO_UNUSED;
  config.gpio_cfg.din = I2S_GPIO_UNUSED;
  if (result == ESP_OK) result = i2s_channel_init_std_mode(rx, &config);
  const unsigned data_pin = microphone ? 16 : 14;
  const unsigned pins[] = {17, 15, data_pin};
  uint32_t mux[3];
  for (unsigned i = 0; i < 3; ++i) {
    mux[i] = REG_READ(GPIO_PIN_MUX_REG[pins[i]]);
    gpio_input_enable(gpio_num_t(pins[i]));
  }
  if (microphone) {
    gpio_config_t pin = {};
    pin.pin_bit_mask = 1ULL << 16;
    pin.mode = GPIO_MODE_INPUT;
    gpio_config(&pin);
  }
  esp_rom_gpio_connect_in_signal(17, I2S1I_BCK_IN_IDX, false);
  esp_rom_gpio_connect_in_signal(15, I2S1I_WS_IN_IDX, false);
  esp_rom_gpio_connect_in_signal(data_pin, I2S1I_SD_IN_IDX, false);
  if (result == ESP_OK) result = i2s_channel_enable(rx);
  const bool started = result == ESP_OK && (calibration ?
      StickSoundBenchTone() : StickForegroundBenchMoveScore());
  const uint8_t regs[] = {0x01, 0x0a, 0x0e, 0x14, 0x17, 0x1c, 0x44};
  // DACL in left slot is a digital control; ADC in right slot is acoustic.
  const uint8_t values[] = {0xbf, 0x0c, 0x02, 0x10, 0xbf, 0x6a, 0x40};
  uint8_t saved[sizeof(regs)] = {};
  bool ok = started;
  if (muted && started)
    ok = StickBoardPower().setGPIOOutput(m5::M5PM1_Class::gpio3, false) && ok;
  if (microphone && started) {
    for (unsigned i = 0; i < sizeof(regs); ++i) {
      ok = m5::In_I2C.readRegister(0x18, regs[i], &saved[i], 1, 100000) && ok;
      ok = m5::In_I2C.writeRegister8(0x18, regs[i], values[i], 100000) && ok;
    }
  }
  struct Bin { uint64_t square, right_square; int64_t sum, right_sum; unsigned count, crossings; float s1, s2; };
  static Bin bins[100];
  std::memset(bins, 0, sizeof(bins));
  bool last_sign = false, sign_valid = false;
  static int16_t left_note[512], right_note[512];
  unsigned note_count = 0;
  bool collecting = false;
  const unsigned bin_count = calibration ? 100 : 30;
  const float coefficient = 2 * std::cos(2 * 3.141592653589793f *
                            // The small speaker's strongest native square-wave
                            // component is its fifth harmonic, verified by FFT.
                            (calibration ? 880.0f : 5 * 32768.0f / 46) / 22050);
  const int64_t start = esp_timer_get_time();
  int16_t samples[256];
  while (ok && esp_timer_get_time() - start < bin_count * 10000) {
    StickSoundService();
    size_t bytes = 0;
    const auto read = i2s_channel_read(rx, samples, sizeof(samples), &bytes, 1);
    if (read != ESP_OK && read != ESP_ERR_TIMEOUT) { result = read; break; }
    for (unsigned i = 0; i < bytes / sizeof(int16_t); i += 2) {
      unsigned index = (esp_timer_get_time() - start) / 10000;
      if (index >= bin_count) break;
      const int64_t value = samples[i];
      bins[index].sum += value;
      bins[index].square += value * value;
      if (value > 100 || value < -100) {
        const bool sign = value > 0;
        bins[index].crossings += sign_valid && sign != last_sign;
        last_sign = sign;
        sign_valid = true;
      }
      const int64_t right = samples[i + 1];
      if (std::abs(int(value)) > 1000 && std::abs(int(value)) < 3000) collecting = true;
      if (collecting && note_count < 512) {
        left_note[note_count] = int16_t(value);
        right_note[note_count++] = int16_t(right);
      }
      bins[index].right_sum += right;
      bins[index].right_square += right * right;
      const float s = float(right) + coefficient * bins[index].s1 - bins[index].s2;
      bins[index].s2 = bins[index].s1;
      bins[index].s1 = s;
      ++bins[index].count;
    }
  }
  if (microphone && started)
    for (unsigned i = 0; i < sizeof(regs); ++i)
      ok = m5::In_I2C.writeRegister8(0x18, regs[i], saved[i], 100000) && ok;
  if (rx) { i2s_channel_disable(rx); i2s_del_channel(rx); }
  if (calibration || muted) StickSoundQuiesceForIr();
  for (unsigned i = 0; i < 3; ++i) REG_WRITE(GPIO_PIN_MUX_REG[pins[i]], mux[i]);
  Serial.printf("PW_STICK_SOUND_CAPTURE mic=%u muted=%u started=%u ok=%u error=%d bins=",
                microphone, muted, started, ok, int(result));
  for (unsigned i = 0; i < bin_count; ++i) {
    const auto &bin = bins[i];
    const double mean = bin.count ? double(bin.sum) / bin.count : 0;
    const double variance = bin.count ? double(bin.square) / bin.count - mean * mean : 0;
    const double right_mean = bin.count ? double(bin.right_sum) / bin.count : 0;
    const double right_variance = bin.count ? double(bin.right_square) / bin.count - right_mean * right_mean : 0;
    const float band = bin.s1 * bin.s1 + bin.s2 * bin.s2 - coefficient * bin.s1 * bin.s2;
    Serial.printf("%u:%u:%u:%u:%u,", bin.count, unsigned(std::sqrt(variance > 0 ? variance : 0)),
                  unsigned(std::sqrt(right_variance > 0 ? right_variance : 0)),
                  bin.count ? unsigned(2 * std::sqrt(band > 0 ? band : 0) / bin.count) : 0,
                  bin.crossings);
  }
  Serial.println();
  Serial.printf("PW_STICK_SOUND_SPECTRUM frames=%u left_bin=%u right_bin=%u bin_hz=43.066406\n",
                note_count, peak_bin(left_note, note_count), peak_bin(right_note, note_count));
  std::memset(left_note, 0, sizeof(left_note));
  std::memset(right_note, 0, sizeof(right_note));
  std::memset(samples, 0, sizeof(samples));
}
#endif
