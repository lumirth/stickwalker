#include "diode_presence.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <driver/touch_sens.h>
#include <esp_timer.h>
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

bool IRAM_ATTR timed_out(touch_sensor_handle_t,
                        const touch_timeout_event_data_t *, void *context)
{
  *static_cast<volatile bool *>(context) = true;
  return false;
}

bool measure(unsigned pin, unsigned speed, unsigned window, unsigned cap,
             uint32_t (&values)[2], int64_t deadline)
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
  uint32_t sums[2] = {}, minimum[2] = {UINT32_MAX, UINT32_MAX};
  uint32_t maximum[2] = {};
  // Discard settling scans before comparing the three retained samples.
  for (unsigned scan = 0; scan < 5 && error == ESP_OK; ++scan) {
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
      sums[i] += raw[i];
      minimum[i] = raw[i] < minimum[i] ? raw[i] : minimum[i];
      maximum[i] = raw[i] > maximum[i] ? raw[i] : maximum[i];
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
    values[i] = sums[i] / 3;
    // Fast, bare pads have a quantization floor of roughly one counter tick
    // per charge cycle. Do not reject them using only a relative error bound.
    if (maximum[i] - minimum[i] > values[i] / 25 + 32) {
#ifdef PW_STICK_BENCH_CONTROL
      Serial.printf("PW_STICK_DIODE_NOISE pin=%u speed=%u window=%u cap=%u "
                    "sample=%u min=%u max=%u average=%u\n", pin, speed,
                    window, cap, i, minimum[i], maximum[i], values[i]);
#endif
      return false;
    }
  }
  return true;
}

pw_stick::DiodeState profile(unsigned sensor_pin, unsigned speed, int64_t deadline)
{
  pw_stick::DiodeCapWindow windows[2];
  for (unsigned window = 0; window < 2; ++window) {
    uint32_t small[2][2], large[2][2];
    for (unsigned i = 0; i < 2; ++i) {
      // One enabled pad per controller avoids mixing partially completed
      // S3 software scans. The same path produced the original live traces.
      const unsigned pin = i ? sensor_pin : 4;
      if (!measure(pin, speed, window, 0, small[i], deadline) ||
          !measure(pin, speed, window, 7, large[i], deadline))
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
  return pw_stick::diode_cap_state(windows);
}
} // namespace

namespace pw_stick {
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
