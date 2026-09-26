#include "board_hal.h"
#include "backlight.h"
#include "peripheral_power.h"

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
struct RestorablePanel : lgfx::Panel_ST7789 {
  bool deferred = false;
  mutable uint8_t prefix[128];
  const uint8_t *getInitCommands(uint8_t list) const override {
    const uint8_t *source = lgfx::Panel_ST7789::getInitCommands(list);
    if (!deferred || !source) return source;
    unsigned used = 0;
    // Reuse the installed driver's exact analog/gamma configuration, but
    // stop before Sleep-out. Its reset and 130 ms delays run as deadlines.
    while (source[0] != CMD_SLPOUT && source[0] != 0xff) {
      const unsigned bytes = 2 + (source[1] & 0x7fu) +
                             ((source[1] & CMD_INIT_DELAY) ? 1u : 0u);
      if (used + bytes + 2 > sizeof(prefix)) return nullptr;
      for (unsigned i = 0; i < bytes; ++i) prefix[used++] = *source++;
    }
    prefix[used++] = 0xff;
    prefix[used] = 0xff;
    return prefix;
  }
} panel;
struct RetainedLight : lgfx::ILight {
  bool init(uint8_t brightness) override {
    return StickBacklightInit(brightness);
  }
  void setBrightness(uint8_t brightness) override {
    StickBacklightSet(brightness);
  }
} light;
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
      // The original PM1 GPIO1 IRQ output is unused: L is polled through the
      // latched button register. With both button reset/off actions disabled,
      // that IRQ function also makes the PM1 flash its status LED repeatedly.
      !pm1.setGPIOFunction(P::gpio1, P::gpio) ||
      !pm1.setGPIOMode(P::gpio1, P::input) ||
      !pm1.setLedEnLevel(false) ||
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
  panel.setLight(&light);
  screen.setPanel(&panel);
  screen_ready = screen.init();
  if (screen_ready) screen.setBrightness(40);
  StickPeripheralPowerInit();
  return screen_ready;
}

bool StickBoardPeripheralSupply(bool on) {
  if (!pm1.setGPIOOutput(m5::M5PM1_Class::gpio2, on)) return false;
  if (!on) {
    // The last owner has already stopped SPI/PWM/I2S. Disconnect output
    // pads to avoid feeding unpowered peripherals through their signal pins.
    // The sensor I2C bus remains available, as in the board's L2 power mode.
    for (int pin : {14, 15, 17, 18, 21, 38, 39, 40, 41, 45})
      gpio_reset_pin(gpio_num_t(pin));
  }
  return true;
}

void StickBoardDisplayReset(bool released) {
  gpio_set_level(GPIO_NUM_21, released);
  gpio_set_direction(GPIO_NUM_21, GPIO_MODE_OUTPUT);
}

bool StickBoardDisplayInitRegisters(void) {
  panel.deferred = true;
  const bool ok = panel.init(false);
  panel.deferred = false;
  return ok;
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
