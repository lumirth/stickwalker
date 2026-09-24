#ifndef PW_STICK_IR_GATE_H
#define PW_STICK_IR_GATE_H

#include <stdint.h>

namespace pw_stick {

struct GateHealth {
  uint32_t phase_late = 0;
  uint32_t already_low = 0;
  uint32_t timed_out = 0;
};

// A single physical measurement on GPIO5. The caller owns the dedicated
// GPIO input bundle, 240-MHz CPU-0 scheduling, and the output-high reset state.
// t0 is the current 3x UART gate origin; next is the following gate origin.
// Keep the electrical transition path self-contained and inspect its generated
// machine code before changing any release/readout/pulldown timing.
uint8_t measure_ir_gate(uint32_t t0, uint32_t next, uint32_t input_mask,
                        GateHealth &health, uint8_t *destination);

}  // namespace pw_stick

#endif
