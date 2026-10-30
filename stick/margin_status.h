#pragma once
#include <stdint.h>
void StickMarginInvalidate();
void StickMarginService();
void StickMarginObservePower(int plugged);
void StickMarginPausePowerEvents();
void StickMarginDraw(bool preflight = false);
uint64_t StickMarginNextDeadline();
