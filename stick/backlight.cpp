#include "backlight.h"

#include <driver/gpio.h>
#include <driver/ledc.h>

namespace {
bool ready = false;
bool timer_configured = false;
bool channel_configured = false;
constexpr auto kMode = LEDC_LOW_SPEED_MODE;
constexpr auto kTimer = LEDC_TIMER_3;
constexpr auto kChannel = LEDC_CHANNEL_7;

unsigned duty(uint8_t brightness) {
  // Preserve the verified LGFX brightness curve, including its offset.
  if (!brightness) return 0;
  constexpr unsigned offset = 16 * 259 >> 8;
  return (brightness * (257 - offset) + offset * 255 + 64) >> 7;
}
}  // namespace

bool StickBacklightInit(uint8_t brightness) {
  ledc_timer_config_t timer = {};
  timer.speed_mode = kMode;
  timer.timer_num = kTimer;
  timer.duty_resolution = LEDC_TIMER_9_BIT;
  timer.freq_hz = 256;
  timer.clk_cfg = LEDC_USE_RC_FAST_CLK;
  ready = ledc_timer_config(&timer) == ESP_OK;
  if (!ready) return false;
  timer_configured = true;
  ledc_channel_config_t channel = {};
  channel.gpio_num = 38;
  channel.speed_mode = kMode;
  channel.channel = kChannel;
  channel.timer_sel = kTimer;
  channel.intr_type = LEDC_INTR_DISABLE;
  channel.duty = duty(brightness);
  channel.sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE;
  ready = ledc_channel_config(&channel) == ESP_OK;
  if (ready) channel_configured = true;
  return ready;
}

void StickBacklightSet(uint8_t brightness) {
  if (!ready && !StickBacklightInit(brightness)) return;
  if (ledc_set_duty(kMode, kChannel, duty(brightness)) != ESP_OK ||
      ledc_update_duty(kMode, kChannel) != ESP_OK) ready = false;
}

bool StickBacklightSleepReady(void) { return ready; }

void StickBacklightSuspend(void) {
  // A duty-update failure clears ready without releasing the hardware.
  // Track allocation separately so that failure cannot skip idle teardown.
  if (channel_configured) {
    ledc_stop(kMode, kChannel, 0);
    channel_configured = false;
  }
  if (timer_configured) {
    ledc_timer_pause(kMode, kTimer);
    ledc_timer_config_t timer = {};
    timer.speed_mode = kMode;
    timer.timer_num = kTimer;
    timer.deconfigure = true;
    if (ledc_timer_config(&timer) == ESP_OK) timer_configured = false;
  }
  ready = false;
}
