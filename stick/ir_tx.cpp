#include "ir_tx.h"
#include "board_hal.h"

#include <M5Unified.h>
#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>
#include <esp_heap_caps.h>

namespace pw_stick {
namespace {

constexpr unsigned kMaxBytes = 136;
constexpr unsigned kBaud = 115200;
constexpr unsigned kRmtHz = 80000000;
constexpr unsigned kPulseTicks = 130;  // 1.625 us at the RMT clock.

rmt_channel_handle_t channel = nullptr;
rmt_encoder_handle_t encoder = nullptr;
rmt_symbol_word_t *symbols = nullptr;
bool ready = false;

}  // namespace

bool prepare_ir_tx() {
  // The 5 V IR rail is disabled outside a session. Restore it before either
  // transmit or receive setup, including when the RMT channel already exists.
  auto &power = StickBoardPower();
  const bool was_off = !power.getExtOutput();
  if (!power.setExtOutput(true)) return false;
  if (was_off) delay(20);
  if (ready) return true;
  if (channel && encoder) {
    ready = rmt_enable(channel) == ESP_OK;
    return ready;
  }
  gpio_set_level(GPIO_NUM_46, 0);
  gpio_set_direction(GPIO_NUM_46, GPIO_MODE_OUTPUT);
  if (!symbols) symbols = static_cast<rmt_symbol_word_t *>(heap_caps_malloc(
      kMaxBytes * 10 * sizeof(rmt_symbol_word_t),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!symbols) return false;
  rmt_tx_channel_config_t channel_config = {};
  channel_config.gpio_num = GPIO_NUM_46;
  channel_config.clk_src = RMT_CLK_SRC_DEFAULT;
  channel_config.resolution_hz = kRmtHz;
  channel_config.mem_block_symbols = 48;
  channel_config.trans_queue_depth = 1;
  rmt_copy_encoder_config_t encoder_config = {};
  esp_err_t result = rmt_new_tx_channel(&channel_config, &channel);
  if (result == ESP_OK) result = rmt_new_copy_encoder(&encoder_config, &encoder);
  if (result == ESP_OK) result = rmt_enable(channel);
  ready = result == ESP_OK;
  if (!ready) {
    if (encoder) { rmt_del_encoder(encoder); encoder = nullptr; }
    if (channel) { rmt_del_channel(channel); channel = nullptr; }
  }
  return ready;
}

bool suspend_ir_tx() {
  if (!ready) return true;
  if (rmt_disable(channel) != ESP_OK) return false;
  ready = false;
  return true;
}

bool transmit_logical(const uint8_t *bytes, size_t length) {
  if (!ready || !bytes || !length || length > kMaxBytes) return false;
  unsigned used = 0;
  unsigned remainder = 0;
  for (size_t index = 0; index < length; ++index) {
    const uint8_t wire = bytes[index] ^ 0xaa;
    for (unsigned bit = 0; bit < 10; ++bit) {
      const bool mark = bit == 9 ||
                        (bit > 0 && (wire & (1u << (bit - 1))));
      unsigned cell = kRmtHz / kBaud;
      remainder += kRmtHz % kBaud;
      if (remainder >= kBaud) {
        remainder -= kBaud;
        ++cell;
      }
      rmt_symbol_word_t &symbol = symbols[used++];
      symbol.val = 0;
      symbol.level0 = mark ? 0 : 1;
      symbol.duration0 = kPulseTicks;
      symbol.level1 = 0;
      symbol.duration1 = cell - kPulseTicks;
    }
  }
  rmt_transmit_config_t tx_config = {};
  tx_config.flags.eot_level = 0;
  esp_err_t result = rmt_transmit(channel, encoder, symbols,
                                  used * sizeof(rmt_symbol_word_t), &tx_config);
  if (result == ESP_OK)
    result = rmt_tx_wait_all_done(channel, 1000);
  if (result != ESP_OK) gpio_set_level(GPIO_NUM_46, 0);
  return result == ESP_OK;
}

}  // namespace pw_stick
