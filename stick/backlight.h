#pragma once

#include <stdint.h>

bool StickBacklightInit(uint8_t brightness);
void StickBacklightSet(uint8_t brightness);
bool StickBacklightSleepReady(void);
void StickBacklightSuspend(void);
