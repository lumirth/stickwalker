#include "ir_tx.h"
#include "board_hal.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <driver/gpio.h>
#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

namespace pw_stick {
namespace {

constexpr unsigned kMaxBytes = 136;
constexpr unsigned kBaud = 115200;
constexpr unsigned kRmtHz = 80000000;
constexpr unsigned kPulseTicks = 130;
constexpr unsigned kDmaSymbols = 1536;
static_assert(kDmaSymbols > kMaxBytes * 10, "DMA must contain the frame and EOF");

rmt_channel_handle_t channel = nullptr;
rmt_encoder_handle_t copy_encoder = nullptr;
rmt_symbol_word_t *symbols = nullptr;
bool ready = false;
bool completed = false;
int64_t completed_us = 0;
unsigned encoding_calls = 0;
bool whole_frame_encoded = false;
int64_t encoded_us = 0;

size_t IRAM_ATTR encode_frame(rmt_encoder_t *, rmt_channel_handle_t tx_channel,
                              const void *data, size_t size,
                              rmt_encode_state_t *state) {
  const size_t count = copy_encoder->encode(copy_encoder, tx_channel, data,
                                             size, state);
  // The public encoding callback runs before EOF insertion, GDMA start and
  // RMT launch. Timestamp after the entire copy so frame length cannot widen
  // the launch bracket. Interrupt delays after here remain visible in it.
  encoded_us = esp_timer_get_time();
  ++encoding_calls;
  whole_frame_encoded = (*state & RMT_ENCODING_COMPLETE) &&
      !(*state & RMT_ENCODING_MEM_FULL) &&
      count * sizeof(rmt_symbol_word_t) == size;
  return count;
}

esp_err_t IRAM_ATTR reset_frame(rmt_encoder_t *) {
  return rmt_encoder_reset(copy_encoder);
}

esp_err_t delete_frame(rmt_encoder_t *) {
  const esp_err_t result = rmt_del_encoder(copy_encoder);
  if (result == ESP_OK) copy_encoder = nullptr;
  return result;
}

rmt_encoder_t encoder = {encode_frame, reset_frame, delete_frame};

bool IRAM_ATTR tx_done(rmt_channel_handle_t, const rmt_tx_done_event_data_t *,
                       void *) {
  completed_us = esp_timer_get_time();
  __atomic_store_n(&completed, true, __ATOMIC_RELEASE);
  return false;
}

}  // namespace

bool prepare_ir_tx() {
  auto &power = StickBoardPower();
  const bool was_off = !power.getExtOutput();
  if (!power.setExtOutput(true)) return false;
  if (was_off) delay(20);
  if (ready) return true;
  if (channel) {
    ready = rmt_enable(channel) == ESP_OK;
    return ready;
  }
  gpio_set_level(GPIO_NUM_46, 0);
  gpio_set_direction(GPIO_NUM_46, GPIO_MODE_OUTPUT);
  if (!symbols) symbols = static_cast<rmt_symbol_word_t *>(heap_caps_malloc(
      kMaxBytes * 10 * sizeof(rmt_symbol_word_t),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!symbols) return false;

  rmt_tx_channel_config_t config = {};
  config.gpio_num = GPIO_NUM_46;
  config.clk_src = RMT_CLK_SRC_DEFAULT;
  config.resolution_hz = kRmtHz;
  config.mem_block_symbols = kDmaSymbols;
  config.trans_queue_depth = 1;
  config.intr_priority = 3;
  config.flags.with_dma = true;
  rmt_copy_encoder_config_t copy_config = {};
  rmt_tx_event_callbacks_t callbacks = {};
  callbacks.on_trans_done = tx_done;
  esp_err_t result = rmt_new_tx_channel(&config, &channel);
  if (result == ESP_OK) result = rmt_new_copy_encoder(&copy_config, &copy_encoder);
  if (result == ESP_OK)
    result = rmt_tx_register_event_callbacks(channel, &callbacks, nullptr);
  if (result == ESP_OK) result = rmt_enable(channel);
  ready = result == ESP_OK;
  if (!ready) {
    if (copy_encoder) delete_frame(&encoder);
    if (channel) { rmt_del_channel(channel); channel = nullptr; }
  }
  return ready;
}

bool suspend_ir_tx() {
  if (!ready) return true;
  if (rmt_disable(channel) != ESP_OK) return false;
  ready = false;
  // Disabling an interrupted transaction moves it to the completed queue.
  // Recycle it now so a subsequent session starts with an empty queue.
  return rmt_tx_wait_all_done(channel, 0) == ESP_OK;
}

bool transmit_logical(const uint8_t *bytes, size_t length, TxWindow &window) {
  window = {};
  if (!ready || !bytes || !length || length > kMaxBytes) return false;
  unsigned used = 0, remainder = 0, ticks = 0;
  for (size_t index = 0; index < length; ++index) {
    const uint8_t wire = bytes[index] ^ 0xaa;
    for (unsigned bit = 0; bit < 10; ++bit) {
      const bool dark = bit == 9 ||
                        (bit > 0 && (wire & (1u << (bit - 1))));
      unsigned cell = kRmtHz / kBaud;
      remainder += kRmtHz % kBaud;
      if (remainder >= kBaud) { remainder -= kBaud; ++cell; }
      rmt_symbol_word_t &symbol = symbols[used++];
      symbol.val = 0;
      symbol.level0 = dark ? 0 : 1;
      symbol.duration0 = kPulseTicks;
      symbol.level1 = 0;
      symbol.duration1 = cell - kPulseTicks;
      ticks += cell;
    }
  }

  // This driver admits exactly one transaction at a time. In the pinned IDF,
  // rmt_transmit preloads the whole DMA buffer and starts an idle channel
  // synchronously. No encoder refill or task scheduling is needed mid-frame.
  __atomic_store_n(&completed, false, __ATOMIC_RELEASE);
  encoding_calls = 0;
  whole_frame_encoded = false;
  rmt_transmit_config_t config = {};
  config.flags.eot_level = 0;
  config.flags.queue_nonblocking = true;
  const esp_err_t result = rmt_transmit(channel, &encoder, symbols,
      used * sizeof(rmt_symbol_word_t), &config);
  const int64_t after = esp_timer_get_time();
  if (result != ESP_OK) return false;
  if (encoding_calls != 1 || !whole_frame_encoded) {
    suspend_ir_tx();
    gpio_set_level(GPIO_NUM_46, 0);
    return false;
  }

  window.earliest_end_us = encoded_us + ticks / 80;
  window.latest_end_us = after + (ticks + 79) / 80 + 1;
  const int64_t deadline = window.latest_end_us + 2000;
  while (!__atomic_load_n(&completed, __ATOMIC_ACQUIRE) &&
         esp_timer_get_time() < deadline) {}
  if (!__atomic_load_n(&completed, __ATOMIC_ACQUIRE)) {
    suspend_ir_tx();
    gpio_set_level(GPIO_NUM_46, 0);
    return false;
  }
  // ISR delivery can be late. Its timestamp supplies another upper bound;
  // lateness cannot move the boundary beyond the start/duration bound.
  if (completed_us + 1 < window.latest_end_us)
    window.latest_end_us = completed_us + 1;
  if (window.latest_end_us < window.earliest_end_us ||
      rmt_tx_wait_all_done(channel, 0) != ESP_OK) return false;
  return true;
}

}  // namespace pw_stick
