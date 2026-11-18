#include "diode.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/touch_sens.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "hal/touch_sensor_ll.h"
#include "soc/gpio_struct.h"

typedef struct { int up, both, down; } DiodeBiasWindow;

static DiodeIcon diode_bias_state(const DiodeBiasWindow windows[3])
{
    int minimum = 10000, maximum = 0, low = 10000, high = 0;
    bool junction = true, photocurrent = true;
    for (unsigned i = 0; i < 3; ++i) {
        const DiodeBiasWindow *w = &windows[i];
        if (w->up < minimum) minimum = w->up;
        if (w->up > maximum) maximum = w->up;
        if (w->down < low) low = w->down;
        if (w->down > high) high = w->down;
        photocurrent &= w->down >= 200 && w->down <= 800;
        junction &= w->up >= 400 && w->up <= 1000 && w->down >= -30 &&
                    w->down <= 100 && w->up - w->both >= -20 && w->up - w->both <= 35;
    }
    return (photocurrent && high - low <= 50) || (junction && maximum - minimum <= 40)
        ? DIODE_REVERSED : DIODE_HIDDEN;
}

static void restore(unsigned pin)
{
  const gpio_num_t gpio = (gpio_num_t)pin;
  rtc_gpio_pullup_dis(gpio);
  rtc_gpio_pulldown_dis(gpio);
  rtc_gpio_deinit(gpio);
  gpio_reset_pin(gpio);
  gpio_set_direction(gpio, GPIO_MODE_INPUT);
  gpio_pullup_dis(gpio);
  gpio_pulldown_dis(gpio);
}

static bool can_charge(unsigned pin)
{
  const gpio_num_t gpio = (gpio_num_t)pin;
  gpio_set_direction(gpio, GPIO_MODE_INPUT);
  gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
  unsigned highs = 0;
  for (unsigned sample = 0; sample < 32; ++sample) {
    esp_rom_delay_us(103 + (sample * 37) % 89);
    highs = gpio_get_level(gpio) ? highs + 1 : 0;
    if (highs == 4) break;
  }
  gpio_set_pull_mode(gpio, GPIO_FLOATING);
  return highs == 4;
}

static DiodeIcon bias_state(unsigned pin)
{
  adc_oneshot_unit_handle_t unit = NULL;
  adc_cali_handle_t calibration = NULL;
  const gpio_num_t gpio = (gpio_num_t)pin;
  const adc_channel_t adc_channel = (adc_channel_t)(pin - 1);
  adc_oneshot_unit_init_cfg_t init = {0};
  init.unit_id = ADC_UNIT_1;
  adc_oneshot_chan_cfg_t channel = {0};
  channel.atten = ADC_ATTEN_DB_12;
  channel.bitwidth = ADC_BITWIDTH_12;
  adc_cali_curve_fitting_config_t config = {0};
  config.unit_id = ADC_UNIT_1;
  config.chan = adc_channel;
  config.atten = channel.atten;
  config.bitwidth = channel.bitwidth;
  DiodeIcon state = DIODE_HIDDEN;
  bool ok = adc_oneshot_new_unit(&init, &unit) == ESP_OK;
  if (ok) ok = adc_oneshot_config_channel(unit, adc_channel, &channel) == ESP_OK;
  if (ok) ok = adc_cali_create_scheme_curve_fitting(&config, &calibration) == ESP_OK;
  DiodeBiasWindow windows[3] = {0};
  // ADC setup selects the digital mux on S3. Control its digital weak pulls,
  // not the independent RTC pulls. Reverse sweep order to catch changing light.
  for (unsigned sweep = 0; sweep < 3 && ok; ++sweep) {
    const gpio_pull_mode_t modes[] = {GPIO_PULLUP_ONLY, GPIO_PULLUP_PULLDOWN,
                                     GPIO_PULLDOWN_ONLY};
    for (unsigned step = 0; step < 3 && ok; ++step) {
      const unsigned bias = sweep & 1 ? 2 - step : step;
      ok = gpio_set_pull_mode(gpio, modes[bias]) == ESP_OK;
      esp_rom_delay_us(300);
      int raw = 0, sum = 0;
      if (ok) ok = adc_oneshot_read(unit, adc_channel, &raw) == ESP_OK;
      for (unsigned sample = 0; sample < 8 && ok; ++sample) {
        int mv = 0;
        ok = adc_oneshot_read(unit, adc_channel, &raw) == ESP_OK &&
             adc_cali_raw_to_voltage(calibration, raw, &mv) == ESP_OK;
        sum += mv;
        esp_rom_delay_us(31);
      }
      int *values[] = {&windows[sweep].up, &windows[sweep].both,
                      &windows[sweep].down};
      *values[bias] = sum / 8;
    }
  }
  if (ok) state = diode_bias_state(windows);
  if (calibration) adc_cali_delete_scheme_curve_fitting(calibration);
  if (unit) adc_oneshot_del_unit(unit);
  restore(pin);
  return state;
}

static bool IRAM_ATTR timed_out(touch_sensor_handle_t controller,
                        const touch_timeout_event_data_t *event, void *context)
{
  (void)controller;
  (void)event;
  *(volatile bool *)context = true;
  return false;
}

static bool measure(unsigned pin, unsigned speed, unsigned window, unsigned cap,
             uint32_t values[2], bool *noisy, int64_t deadline)
{
  touch_sensor_handle_t controller = NULL;
  touch_channel_handle_t channel = NULL;
  bool enabled = false;
  volatile bool timeout = false;
  restore(pin);
  touch_sensor_sample_config_t sample = {0};
  sample.charge_times = 32;
  sample.charge_volt_lim_l =
      window ? TOUCH_VOLT_LIM_L_0V8 : TOUCH_VOLT_LIM_L_0V5;
  sample.charge_volt_lim_h =
      window ? TOUCH_VOLT_LIM_H_2V4 : TOUCH_VOLT_LIM_H_0V9;
  sample.idle_conn = TOUCH_IDLE_CONN_HIGHZ;
  sample.bias_type = TOUCH_BIAS_TYPE_BANDGAP;
  touch_sensor_config_t config = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, &sample);
  config.max_meas_time_us = 3000;
  touch_channel_config_t pad = {0};
  pad.charge_speed = (touch_charge_speed_t)speed;
  pad.init_charge_volt = TOUCH_INIT_CHARGE_VOLT_FLOAT;
  touch_denoise_chan_config_t denoise = {0};
  denoise.charge_speed = pad.charge_speed;
  denoise.init_charge_volt = pad.init_charge_volt;
  denoise.ref_cap = (touch_denoise_chan_cap_t)cap;
  denoise.resolution = TOUCH_DENOISE_CHAN_RESOLUTION_BIT4;
  touch_event_callbacks_t callbacks = {0};
  callbacks.on_timeout = timed_out;

  esp_err_t error = touch_sensor_new_controller(&config, &controller);
  if (error == ESP_OK)
    error = touch_sensor_new_channel(controller, pin, &pad, &channel);
  if (error == ESP_OK)
    error = touch_sensor_config_denoise_channel(controller, &denoise);
  if (error == ESP_OK)
    error = touch_sensor_register_callbacks(controller, &callbacks,
                                           (void *)&timeout);
  if (error == ESP_OK) {
    error = touch_sensor_enable(controller);
    enabled = error == ESP_OK;
  }
  uint32_t samples[2][5] = {0};
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
    uint32_t raw[2] = {0};
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
    *noisy |= variable;
  }
  return true;
}


static DiodeIcon capacitance(int64_t deadline)
{
    int64_t added[2];
    bool noisy = false;
    for (unsigned window = 0; window < 2; ++window) {
        uint32_t small[2][2], large[2][2];
        for (unsigned i = 0; i < 2; ++i) {
            unsigned pin = i ? 5 : 4;
            if (!measure(pin, 3, window, 0, small[i], &noisy, deadline) ||
                !measure(pin, 3, window, 7, large[i], &noisy, deadline)) return DIODE_HIDDEN;
            unsigned difference = small[i][0] > large[i][0]
                ? small[i][0] - large[i][0] : large[i][0] - small[i][0];
            if (difference > (small[i][0] + large[i][0]) / 50 + 32) return DIODE_HIDDEN;
        }
        int64_t reference = (small[0][0] + large[0][0]) / 2;
        int64_t diode = (small[1][0] + large[1][0]) / 2;
        int64_t low = (small[0][1] + small[1][1]) / 2;
        int64_t high = (large[0][1] + large[1][1]) / 2;
        if (high <= low + 128) return DIODE_HIDDEN;
        int64_t bare = 5000 + (reference - low) * 9800 / (high - low);
        if (bare < 7000 || bare > 14000) return DIODE_HIDDEN;
        added[window] = (diode - reference) * 9800 / (high - low);
    }
    int64_t curve = added[0] - added[1];
    // Bounds from the original Stick's touch/reference characterization.
    if (!noisy && added[0] >= -6000 && added[0] <= 1800 &&
        added[1] >= -6000 && added[1] <= 1500 && curve >= -6000 &&
        (curve <= 1200 || added[1] <= -1500)) return DIODE_MISSING;
    if (added[0] >= 4500 && added[0] <= 22000 && added[1] >= 4000 &&
        added[1] <= 18000 && curve >= 700 && curve <= 8000) return DIODE_READY;
    return DIODE_HIDDEN;
}

DiodeProbe diode_probe(void)
{
    restore(5);
    if (!can_charge(5)) return (DiodeProbe){bias_state(5), false};
    DiodeProbe result = {DIODE_HIDDEN, true};
    if (!(GPIO.enable & (1u << 4)) && can_charge(4)) {
        result.icon = capacitance(esp_timer_get_time() + 120000);
        restore(4);
    }
    restore(5);
    return result;
}
