#include "diode_presence.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <driver/touch_sens.h>
#include <esp_timer.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <hal/touch_sensor_ll.h>
#include <soc/gpio_struct.h>

namespace {
void restore(unsigned pin)
{
  const auto gpio = gpio_num_t(pin);
  rtc_gpio_pullup_dis(gpio);
  rtc_gpio_pulldown_dis(gpio);
  rtc_gpio_deinit(gpio);
  gpio_reset_pin(gpio);
  gpio_set_direction(gpio, GPIO_MODE_INPUT);
  gpio_pullup_dis(gpio);
  gpio_pulldown_dis(gpio);
}

bool can_charge(unsigned pin)
{
  const auto gpio = gpio_num_t(pin);
  gpio_set_direction(gpio, GPIO_MODE_INPUT);
  gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
  unsigned highs = 0;
  for (unsigned sample = 0; sample < 32; ++sample) {
    delayMicroseconds(103 + (sample * 37) % 89);
    highs = gpio_get_level(gpio) ? highs + 1 : 0;
    if (highs == 4) break;
  }
  gpio_set_pull_mode(gpio, GPIO_FLOATING);
  return highs == 4;
}

pw_stick::DiodeState bias_state(unsigned pin)
{
  adc_oneshot_unit_handle_t unit = nullptr;
  adc_cali_handle_t calibration = nullptr;
  const auto gpio = gpio_num_t(pin);
  const auto adc_channel = adc_channel_t(pin - 1);
  adc_oneshot_unit_init_cfg_t init = {};
  init.unit_id = ADC_UNIT_1;
  adc_oneshot_chan_cfg_t channel = {};
  channel.atten = ADC_ATTEN_DB_12;
  channel.bitwidth = ADC_BITWIDTH_12;
  adc_cali_curve_fitting_config_t config = {};
  config.unit_id = ADC_UNIT_1;
  config.chan = adc_channel;
  config.atten = channel.atten;
  config.bitwidth = channel.bitwidth;
  pw_stick::DiodeState state = pw_stick::DiodeState::Unknown;
  bool ok = adc_oneshot_new_unit(&init, &unit) == ESP_OK;
  if (ok) ok = adc_oneshot_config_channel(unit, adc_channel, &channel) == ESP_OK;
  if (ok) ok = adc_cali_create_scheme_curve_fitting(&config, &calibration) == ESP_OK;
  pw_stick::DiodeBiasWindow windows[3];
  // ADC setup selects the digital mux on S3. Control its digital weak pulls,
  // not the independent RTC pulls. Reverse sweep order to catch changing light.
  for (unsigned sweep = 0; sweep < 3 && ok; ++sweep) {
    const gpio_pull_mode_t modes[] = {GPIO_PULLUP_ONLY, GPIO_PULLUP_PULLDOWN,
                                     GPIO_PULLDOWN_ONLY};
    for (unsigned step = 0; step < 3 && ok; ++step) {
      const unsigned bias = sweep & 1 ? 2 - step : step;
      ok = gpio_set_pull_mode(gpio, modes[bias]) == ESP_OK;
      delayMicroseconds(300);
      int raw = 0, sum = 0;
      if (ok) ok = adc_oneshot_read(unit, adc_channel, &raw) == ESP_OK;
      for (unsigned sample = 0; sample < 8 && ok; ++sample) {
        int mv = 0;
        ok = adc_oneshot_read(unit, adc_channel, &raw) == ESP_OK &&
             adc_cali_raw_to_voltage(calibration, raw, &mv) == ESP_OK;
        sum += mv;
        delayMicroseconds(31);
      }
      int *values[] = {&windows[sweep].up, &windows[sweep].both,
                      &windows[sweep].down};
      *values[bias] = sum / 8;
    }
#ifdef PW_STICK_BENCH_CONTROL
    Serial.printf("PW_STICK_DIODE_BIAS pin=%u sweep=%u up=%d both=%d down=%d ok=%u\n",
                  pin, sweep, windows[sweep].up, windows[sweep].both,
                  windows[sweep].down, unsigned(ok));
#endif
  }
  if (ok) state = pw_stick::diode_bias_state(windows);
  if (calibration) adc_cali_delete_scheme_curve_fitting(calibration);
  if (unit) adc_oneshot_del_unit(unit);
  restore(pin);
  return state;
}

bool IRAM_ATTR timed_out(touch_sensor_handle_t,
                        const touch_timeout_event_data_t *, void *context)
{
  *static_cast<volatile bool *>(context) = true;
  return false;
}

bool measure(unsigned pin, unsigned speed, unsigned window, unsigned cap,
             uint32_t (&values)[2], bool &noisy, int64_t deadline)
{
  touch_sensor_handle_t controller = nullptr;
  touch_channel_handle_t channel = nullptr;
  bool enabled = false;
  volatile bool timeout = false;
  restore(pin);
  touch_sensor_sample_config_t sample = {};
  sample.charge_times = 32;
  sample.charge_volt_lim_l =
      window ? TOUCH_VOLT_LIM_L_0V8 : TOUCH_VOLT_LIM_L_0V5;
  sample.charge_volt_lim_h =
      window ? TOUCH_VOLT_LIM_H_2V4 : TOUCH_VOLT_LIM_H_0V9;
  sample.idle_conn = TOUCH_IDLE_CONN_HIGHZ;
  sample.bias_type = TOUCH_BIAS_TYPE_BANDGAP;
  touch_sensor_config_t config = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, &sample);
  config.max_meas_time_us = 3000;
  touch_channel_config_t pad = {};
  pad.charge_speed = touch_charge_speed_t(speed);
  pad.init_charge_volt = TOUCH_INIT_CHARGE_VOLT_FLOAT;
  touch_denoise_chan_config_t denoise = {};
  denoise.charge_speed = pad.charge_speed;
  denoise.init_charge_volt = pad.init_charge_volt;
  denoise.ref_cap = touch_denoise_chan_cap_t(cap);
  denoise.resolution = TOUCH_DENOISE_CHAN_RESOLUTION_BIT4;
  touch_event_callbacks_t callbacks = {};
  callbacks.on_timeout = timed_out;

  esp_err_t error = touch_sensor_new_controller(&config, &controller);
  if (error == ESP_OK)
    error = touch_sensor_new_channel(controller, pin, &pad, &channel);
  if (error == ESP_OK)
    error = touch_sensor_config_denoise_channel(controller, &denoise);
  if (error == ESP_OK)
    error = touch_sensor_register_callbacks(controller, &callbacks,
                                           const_cast<bool *>(&timeout));
  if (error == ESP_OK) {
    error = touch_sensor_enable(controller);
    enabled = error == ESP_OK;
  }
  uint32_t samples[2][5] = {};
  // Discard settling scans before comparing five retained samples.
  for (unsigned scan = 0; scan < 7 && error == ESP_OK; ++scan) {
    if (esp_timer_get_time() >= deadline) {
      error = ESP_ERR_TIMEOUT;
      break;
    }
    error = touch_sensor_trigger_oneshot_scanning(controller, 10);
    if (error != ESP_OK || timeout) {
      error = ESP_ERR_TIMEOUT;
      break;
    }
    uint32_t raw[2] = {};
    error = touch_channel_read_data(channel, TOUCH_CHAN_DATA_TYPE_RAW, &raw[0]);
    touch_ll_denoise_read_data(&raw[1]);
    if (scan < 2)
      continue;
    for (unsigned i = 0; i < 2; ++i) {
      if (raw[i] > 1000000 || !raw[i]) {
        error = ESP_ERR_INVALID_RESPONSE;
        break;
      }
      samples[i][scan - 2] = raw[i];
    }
  }
  if (enabled)
    touch_sensor_disable(controller);
  if (channel)
    touch_sensor_del_channel(channel);
  if (controller)
    touch_sensor_del_controller(controller);
  restore(pin);
  if (timeout || error != ESP_OK || esp_timer_get_time() >= deadline)
    return false;
  for (unsigned i = 0; i < 2; ++i) {
    for (unsigned a = 0; a < 5; ++a)
      for (unsigned b = a + 1; b < 5; ++b)
        if (samples[i][b] < samples[i][a]) {
          const uint32_t temporary = samples[i][a];
          samples[i][a] = samples[i][b];
          samples[i][b] = temporary;
        }
    // Interrupted charge cycles can increase counts. Require a repeatable
    // lower cluster, and never diagnose absence from a noisy measurement.
    values[i] = (samples[i][0] + samples[i][1]) / 2;
    const unsigned spread = values[i] / 25 + 32;
    if (samples[i][1] - samples[i][0] > spread) return false;
    const bool variable = samples[i][4] - samples[i][0] > spread;
    if (i == 1 && variable) return false;
    noisy |= variable;
  }
  return true;
}

pw_stick::DiodeState profile(unsigned sensor_pin, unsigned speed, int64_t deadline)
{
  pw_stick::DiodeCapWindow windows[2];
  bool noisy = false;
  for (unsigned window = 0; window < 2; ++window) {
    uint32_t small[2][2], large[2][2];
    for (unsigned i = 0; i < 2; ++i) {
      // One enabled pad per controller avoids mixing partially completed
      // S3 software scans. The same path produced the original live traces.
      const unsigned pin = i ? sensor_pin : 4;
      if (!measure(pin, speed, window, 0, small[i], noisy, deadline) ||
          !measure(pin, speed, window, 7, large[i], noisy, deadline))
        return pw_stick::DiodeState::Unknown;
      const uint32_t difference = small[i][0] > large[i][0]
          ? small[i][0] - large[i][0] : large[i][0] - small[i][0];
      if (difference > (small[i][0] + large[i][0]) / 50 + 32)
        return pw_stick::DiodeState::Unknown;
    }
    auto &w = windows[window];
    w.reference = (small[0][0] + large[0][0]) / 2;
    w.diode = (small[1][0] + large[1][0]) / 2;
    w.internal_low = (small[0][1] + small[1][1]) / 2;
    w.internal_high = (large[0][1] + large[1][1]) / 2;
#ifdef PW_STICK_BENCH_CONTROL
    Serial.printf("PW_STICK_DIODE_CAP speed=%u window=%u diode=%u bare=%u "
                  "internal_low=%u internal_high=%u\n", speed, window,
                  w.diode, w.reference, w.internal_low, w.internal_high);
#endif
  }
  const auto state = pw_stick::diode_cap_state(windows);
  return noisy && state == pw_stick::DiodeState::Missing
      ? pw_stick::DiodeState::Unknown : state;
}
} // namespace

namespace pw_stick {
DiodeProbe diode_probe(unsigned pin)
{
  if (pin < 5 || pin > 8) return {};
  restore(pin);
  if (!can_charge(pin)) return {bias_state(pin), false};
  const DiodeState state = diode_presence_detect(pin);
  return {state, true};
}

DiodeState diode_presence_detect(unsigned sensor_pin)
{
  // GPIO4 is unused by Stickwalker and has a matching exposed Hat trace.
  // A loaded/driven reference must leave the result inconclusive.
  if (sensor_pin < 5 || sensor_pin > 8 || GPIO.enable & (1 << 4))
    return DiodeState::Unknown;
  restore(4);
  restore(sensor_pin);
  gpio_set_direction(GPIO_NUM_4, GPIO_MODE_INPUT);
  gpio_pulldown_dis(GPIO_NUM_4);
  gpio_pullup_en(GPIO_NUM_4);
  delayMicroseconds(300);
  const bool high = gpio_get_level(GPIO_NUM_4);
  gpio_pullup_dis(GPIO_NUM_4);
  if (!high)
    return DiodeState::Unknown;
  const int64_t deadline = esp_timer_get_time() + 150000;
  const DiodeState first = profile(sensor_pin, 3, deadline);
  if (first == DiodeState::Unknown)
    return first;
  const DiodeState second = profile(sensor_pin, 7, deadline);
  return first == second ? first : DiodeState::Unknown;
}
} // namespace pw_stick

#ifdef PW_STICK_BENCH_CONTROL
extern "C" unsigned StickDiodeBiasProbe(void)
{
  return unsigned(bias_state(5));
}
#endif
