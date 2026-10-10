#ifndef PW_STICK_BOARD_HAL_H
#define PW_STICK_BOARD_HAL_H

#include <M5Unified.h>

bool StickBoardBegin(void);
lgfx::LGFX_Device *StickBoardScreen(void);
bool StickBoardAccel(float *x, float *y, float *z);
// Returns false on a failed supply measurement.
bool StickBoardVbusVoltage(uint16_t *millivolts);
// Call through the shared supply manager after display and audio teardown.
bool StickBoardPeripheralSupply(bool on);
void StickBoardDisplayReset(bool released);
bool StickBoardDisplayInitRegisters(void);
#ifdef PW_STICK_BENCH_CONTROL
void StickBoardAccelDiagnostic(unsigned *reads, unsigned *successes,
                               unsigned *failures);
#endif
m5::M5PM1_Class &StickBoardPower(void);

#endif
