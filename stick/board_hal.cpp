#include "board_hal.h"

#include <M5Unified.h>
#include <utility/imu/BMI270_Class.hpp>
#include <driver/gpio.h>
#include <lgfx/v1/panel/Panel_ST7789.hpp>

namespace {

m5::M5PM1_Class pm1;
m5::BMI270_Class imu;
bool imu_ready = false;
#ifdef PW_STICK_BENCH_CONTROL
unsigned accel_reads = 0, accel_successes = 0, accel_failures = 0;
#endif
lgfx::LGFX_Device screen;
lgfx::Bus_SPI bus;
lgfx::Panel_ST7789 panel;
lgfx::Light_PWM light;
bool screen_ready = false;

}  // namespace

bool StickBoardBegin(void) {
  // Reproduce the board setup used by the frozen optical control. M5.begin()
  // also configures peripherals unrelated to the original Pokewalker and
  // changes the GPIO5 detector's measured dark distribution on this board.
  gpio_set_level(GPIO_NUM_46, 0);
  gpio_set_direction(GPIO_NUM_46, GPIO_MODE_OUTPUT);
  gpio_set_direction(GPIO_NUM_5, GPIO_MODE_INPUT);
  gpio_pullup_dis(GPIO_NUM_5);
  gpio_pulldown_dis(GPIO_NUM_5);
  using P = m5::M5PM1_Class;
  if (!m5::In_I2C.begin(I2C_NUM_1, 47, 48) || !pm1.begin() ||
      !pm1.setGPIOOutput(P::gpio3, false) ||
      !pm1.setGPIOFunction(P::gpio3, P::gpio) ||
      !pm1.setGPIODrive(P::gpio3, P::push_pull) ||
      !pm1.setGPIOMode(P::gpio3, P::output) ||
      !pm1.setGPIOFunction(P::gpio0, P::gpio) ||
      !pm1.setGPIOMode(P::gpio0, P::input) ||
      !pm1.setExtOutput(true)) return false;
  delay(1200);

  if (imu.WhoAmI() != 0x24) imu.setAddress(0x68);
  imu_ready = imu.WhoAmI() == 0x24 &&
              imu.begin() != m5::IMU_Base::imu_spec_none;
  // M5Unified's BMI270 startup enables gyro and temperature too. The native
  // Pokewalker application reads only acceleration, so leave its current
  // accelerometer filter and rate intact while disabling unused sensors.
  if (imu_ready)
    imu_ready = imu.writeRegister8(m5::BMI270_Class::PWR_CTRL_ADDR, 0x04);

  if (!pm1.setGPIOOutput(P::gpio2, true) ||
      !pm1.setGPIOFunction(P::gpio2, P::gpio) ||
      !pm1.setGPIODrive(P::gpio2, P::push_pull) ||
      !pm1.setGPIOMode(P::gpio2, P::output)) return false;
  auto b = bus.config();
  b.spi_host = SPI3_HOST;
  b.spi_mode = 0;
  b.spi_3wire = true;
  b.freq_write = 40000000;
  b.freq_read = 16000000;
  b.pin_mosi = 39;
  b.pin_miso = -1;
  b.pin_sclk = 40;
  b.pin_dc = 45;
  bus.config(b);
  panel.setBus(&bus);
  auto p = panel.config();
  p.pin_cs = 41;
  p.pin_rst = 21;
  p.panel_width = 135;
  p.panel_height = 240;
  p.offset_x = 52;
  p.offset_y = 40;
  p.invert = true;
  p.readable = true;
  p.bus_shared = false;
  panel.config(p);
  auto l = light.config();
  l.pin_bl = 38;
  l.pwm_channel = 7;
  l.freq = 256;
  l.offset = 16;
  light.config(l);
  panel.setLight(&light);
  screen.setPanel(&panel);
  screen_ready = screen.init();
  if (screen_ready) screen.setBrightness(40);
  return screen_ready;
}

lgfx::LGFX_Device *StickBoardScreen(void) {
  return screen_ready ? &screen : nullptr;
}

m5::M5PM1_Class &StickBoardPower(void) { return pm1; }

bool StickBoardAccel(float *x, float *y, float *z) {
#ifdef PW_STICK_BENCH_CONTROL
  ++accel_reads;
#endif
  if (!imu_ready) {
#ifdef PW_STICK_BENCH_CONTROL
    ++accel_failures;
#endif
    return false;
  }
  m5::IMU_Base::imu_raw_data_t raw;
  if (!(imu.getImuRawData(&raw) & m5::IMU_Base::imu_spec_accel)) {
#ifdef PW_STICK_BENCH_CONTROL
    ++accel_failures;
#endif
    return false;
  }
#ifdef PW_STICK_BENCH_CONTROL
  ++accel_successes;
#endif
  m5::IMU_Base::imu_convert_param_t scale;
  imu.getConvertParam(&scale);
  *x = raw.accel.x * scale.accel_res;
  *y = raw.accel.y * scale.accel_res;
  *z = raw.accel.z * scale.accel_res;
  return true;
}

#ifdef PW_STICK_BENCH_CONTROL
void StickBoardAccelDiagnostic(unsigned *reads, unsigned *successes,
                               unsigned *failures) {
  *reads = accel_reads;
  *successes = accel_successes;
  *failures = accel_failures;
}
#endif
