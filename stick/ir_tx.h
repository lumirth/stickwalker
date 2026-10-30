#ifndef PW_STICK_IR_TX_H
#define PW_STICK_IR_TX_H

#include <stddef.h>
#include <stdint.h>

namespace pw_stick {

// Both endpoints are conservative bounds on the final stop bit. The upper
// bound comes from hardware-start bracketing and the TX-complete ISR, never
// from the task being scheduled after a wait.
struct TxWindow {
  int64_t earliest_end_us = 0;
  int64_t latest_end_us = 0;
};

bool prepare_ir_tx();
bool suspend_ir_tx();
bool transmit_logical(const uint8_t *bytes, size_t length, TxWindow &window);

}  // namespace pw_stick

#endif
