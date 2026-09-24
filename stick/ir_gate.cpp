#include "ir_gate.h"

#include <esp_attr.h>
#include <esp_cpu.h>
#include <hal/dedic_gpio_cpu_ll.h>
#include <hal/gpio_ll.h>
#include <soc/io_mux_reg.h>
#include <soc/soc.h>

namespace pw_stick {

uint8_t IRAM_ATTR __attribute__((noinline, noclone)) measure_ir_gate(
    uint32_t t0, uint32_t next, uint32_t input_mask, GateHealth &health,
    uint8_t *destination) {
  uint8_t late = 0;
  const uint32_t release = t0 + 65;
  if (int32_t(esp_cpu_get_cycle_count() - release) >= 0) late |= 1;
  while (int32_t(esp_cpu_get_cycle_count() - release) < 0) {}
  gpio_ll_output_disable(&GPIO, 5);

  const uint32_t readout = t0 + 450;
  if (int32_t(esp_cpu_get_cycle_count() - readout) >= 0) late |= 2;
  while (int32_t(esp_cpu_get_cycle_count() - readout) < 0) {}
  uint8_t measurement = 255;
  if (!(dedic_gpio_cpu_ll_read_in() & input_mask)) {
    measurement = 0;
    ++health.already_low;
  } else {
    REG_SET_BIT(IO_MUX_GPIO5_REG, FUN_PD);
    const uint32_t ramp = esp_cpu_get_cycle_count();
    const uint32_t limit = next - 70;
    if (int32_t(ramp - limit) >= 0) late |= 4;
    bool crossed = false;
    for (unsigned group = 0; group < 5 && !crossed; ++group) {
      for (unsigned probe = 0; probe < 8; ++probe) {
        if (!(dedic_gpio_cpu_ll_read_in() & input_mask)) {
          crossed = true;
          break;
        }
      }
      if (int32_t(esp_cpu_get_cycle_count() - limit) >= 0) break;
    }
    if (crossed) {
      const uint32_t cycles = esp_cpu_get_cycle_count() - ramp;
      measurement = uint8_t(cycles < 252 ? cycles + 1 : 253);
    }
    REG_CLR_BIT(IO_MUX_GPIO5_REG, FUN_PD);
    if (measurement == 255) ++health.timed_out;
  }
  gpio_ll_output_enable(&GPIO, 5);
  if (late) {
    measurement = uint8_t(240 | late);
    ++health.phase_late;
  }
  *destination = measurement;
  return measurement;
}

}  // namespace pw_stick
