#ifndef PW_STICK_BOARD_HAL_H
#define PW_STICK_BOARD_HAL_H

#include <M5Unified.h>

bool StickBoardBegin(void);
lgfx::LGFX_Device *StickBoardScreen(void);
bool StickBoardAccel(float *x, float *y, float *z);
m5::M5PM1_Class &StickBoardPower(void);

#endif
