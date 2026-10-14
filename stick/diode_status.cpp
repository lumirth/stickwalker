#include "diode_status.h"

#include "diode_feedback.h"
#include "diode_presence.h"
#include "board_hal.h"
#include "board_settings.h"
#include "display_bus.h"
#include "display_panel.h"
#include "foreground_bridge.h"
#include "input_bridge.h"
#include <Arduino.h>
#include <M5Unified.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_timer.h>

namespace {
using pw_stick::DiodeState;
pw_stick::DiodeFeedback feedback;
bool dirty = false;
bool confirmed = false;
bool blocked = false;
DiodeState presence = DiodeState::Unknown;
DiodeState fault = DiodeState::Hidden;
unsigned rechecks = 0;
uint64_t next_check = UINT64_MAX;
bool was_awake = false;
bool wake_check = false;
void draw();

void restore_pin()
{
  rtc_gpio_pullup_dis(GPIO_NUM_5);
  rtc_gpio_pulldown_dis(GPIO_NUM_5);
  rtc_gpio_deinit(GPIO_NUM_5);
  gpio_reset_pin(GPIO_NUM_5);
  gpio_set_direction(GPIO_NUM_5, GPIO_MODE_INPUT);
  gpio_pullup_dis(GPIO_NUM_5);
  gpio_pulldown_dis(GPIO_NUM_5);
}

DiodeState voltage_state()
{
  adc_oneshot_unit_handle_t unit = nullptr;
  adc_cali_handle_t calibration = nullptr;
  DiodeState result = DiodeState::Unknown;
  adc_oneshot_unit_init_cfg_t init = {};
  init.unit_id = ADC_UNIT_1;
  adc_oneshot_chan_cfg_t channel = {};
  channel.atten = ADC_ATTEN_DB_12;
  channel.bitwidth = ADC_BITWIDTH_12;
  adc_cali_curve_fitting_config_t config = {};
  config.unit_id = ADC_UNIT_1;
  config.chan = ADC_CHANNEL_4;
  config.atten = channel.atten;
  config.bitwidth = channel.bitwidth;
  if (adc_oneshot_new_unit(&init, &unit) == ESP_OK &&
      adc_oneshot_config_channel(unit, ADC_CHANNEL_4, &channel) == ESP_OK &&
      adc_cali_create_scheme_curve_fitting(&config, &calibration) == ESP_OK &&
      rtc_gpio_pullup_dis(GPIO_NUM_5) == ESP_OK &&
      rtc_gpio_pulldown_en(GPIO_NUM_5) == ESP_OK) {
    int values[3] = {};
    bool ok = true;
    for (unsigned index = 0; index < 3 && ok; ++index) {
      delayMicroseconds(173 + index * 47);
      int sum = 0;
      for (unsigned sample = 0; sample < 4; ++sample) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(unit, ADC_CHANNEL_4, &raw) != ESP_OK ||
            adc_cali_raw_to_voltage(calibration, raw, &mv) != ESP_OK) {
          ok = false;
          break;
        }
        sum += mv;
      }
      values[index] = sum / 4;
    }
    if (ok)
      result = pw_stick::diode_voltage_state(values);
  }
  if (calibration)
    adc_cali_delete_scheme_curve_fitting(calibration);
  if (unit)
    adc_oneshot_del_unit(unit);
  restore_pin();
  return result;
}

bool can_charge(gpio_num_t pin = GPIO_NUM_5)
{
  gpio_set_direction(pin, GPIO_MODE_INPUT);
  gpio_pulldown_dis(pin);
  gpio_pullup_en(pin);
  unsigned high_samples = 0;
  // A transmitting peer must have dark gaps between its short SIR pulses.
  // Vary sample spacing to avoid repeatedly landing on the same UART phase.
  for (unsigned sample = 0; sample < 32; ++sample) {
    delayMicroseconds(103 + (sample * 37) % 89);
    high_samples = gpio_get_level(pin) ? high_samples + 1 : 0;
    if (high_samples == 4) {
      break;
    }
  }
  gpio_pullup_dis(pin);
  return high_samples == 4;
}

DiodeState inspect()
{
  return can_charge() ? pw_stick::diode_presence_detect() : voltage_state();
}

void show(DiodeState state)
{
  feedback.show(state, uint64_t(esp_timer_get_time()));
  dirty = true;
}
} // namespace

extern "C" void StickDiodeBegin(void)
{
  confirmed = false;
  blocked = false;
  presence = DiodeState::Unknown;
  fault = DiodeState::Hidden;
  next_check = UINT64_MAX;
  rechecks = 0;
  show(DiodeState::Hidden);
}

extern "C" int StickDiodePreflight(void)
{
  if (can_charge()) {
    presence = pw_stick::diode_presence_detect();
    if (presence == DiodeState::Connected)
      show(presence);
    if (presence == DiodeState::Missing) {
      blocked = true;
      fault = presence;
      show(fault);
      draw();
      return 0;
    }
    draw();
    return 1;
  }
  blocked = true;
  fault = voltage_state();
  show(fault);
  draw();
  return 0;
}

extern "C" void StickDiodeReceivedPacket(void)
{
  confirmed = true;
}

extern "C" void StickDiodeEnd(unsigned)
{
  if (confirmed || presence == DiodeState::Connected) {
    show(DiodeState::Connected);
    return;
  }
  show(fault);
  // Three rechecks, only after a blocked input, only while the screen is awake.
  if (blocked) {
    rechecks = 3;
    next_check = uint64_t(esp_timer_get_time()) + 500000;
  }
}

extern "C" void StickDiodeService(void)
{
  const uint64_t now = uint64_t(esp_timer_get_time());
  const bool awake = StickDisplayIsPowered();
  if (awake && !was_awake)
    wake_check = true;
  was_awake = awake;
  if (!awake || StickSettingsIsOpen()) {
    if (feedback.state != DiodeState::Hidden)
      show(DiodeState::Hidden);
    next_check = UINT64_MAX;
    rechecks = 0;
    return;
  }
  if (StickForegroundIsIr())
    return;
  if (wake_check) {
    wake_check = false;
    const DiodeState state = inspect();
    show(state);
    if (state != DiodeState::Connected) {
      rechecks = 3;
      next_check = uint64_t(esp_timer_get_time()) + 500000;
    }
  }
  if (feedback.expire(now)) {
    dirty = true;
    next_check = UINT64_MAX;
    rechecks = 0;
  }
  if (now < next_check)
    return;
  --rechecks;
  const DiodeState state = inspect();
  if (state == DiodeState::Connected) {
    show(state);
    rechecks = 0;
  } else if (state != feedback.state) {
    show(state);
  }
  next_check = rechecks ? now + 500000 : UINT64_MAX;
}

#ifdef PW_STICK_BENCH_CONTROL
extern "C" unsigned StickDiodeProbe(void)
{
  return unsigned(inspect());
}
extern "C" unsigned StickDiodeProbePin(unsigned pin)
{
  if (pin < 6 || pin > 8 || !can_charge(gpio_num_t(pin)))
    return unsigned(DiodeState::Unknown);
  return unsigned(pw_stick::diode_presence_detect(pin));
}
#endif

extern "C" uint64_t StickDiodeNextDeadline(void)
{
  return next_check < feedback.expires ? next_check : feedback.expires;
}

extern "C" void StickDiodeInvalidate(void)
{
  dirty = true;
}

namespace {
void draw()
{
  if (!dirty || !StickDisplayPanelIsReady() || StickSettingsIsOpen())
    return;
  auto *screen = StickBoardScreen();
  const unsigned x = pw_stick::diode_icon_x(StickInputOrientation());
  const unsigned y = pw_stick::diode_icon_y(StickInputOrientation());
  uint16_t pixels[16 * 16];
  const uint16_t background = StickDisplayBackground();
  const uint16_t foreground = StickDisplayForeground();
  for (unsigned row = 0; row < 16; ++row) {
    const uint16_t bits =
        feedback.state == DiodeState::Hidden
            ? 0
            : pw_stick::kDiodeIcons[unsigned(feedback.state) - 1][row];
    for (unsigned column = 0; column < 16; ++column)
      pixels[row * 16 + column] =
          bits & (1u << column) ? foreground : background;
  }
  screen->pushImage(x, y, 16, 16, pixels);
  dirty = false;
}
} // namespace

extern "C" void StickDiodeDraw(void)
{
  if (!StickForegroundIsIr())
    draw();
}
