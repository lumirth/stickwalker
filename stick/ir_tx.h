#ifndef PW_STICK_IR_TX_H
#define PW_STICK_IR_TX_H

#include <stddef.h>
#include <stdint.h>

namespace pw_stick {

// The caller provides logical Pokewalker bytes. This driver applies the
// original SCI3 transport XOR and emits 115200-baud 8N1 SIR through GPIO46.
bool prepare_ir_tx();
bool suspend_ir_tx();
bool transmit_logical(const uint8_t *bytes, size_t length);

}  // namespace pw_stick

#endif
