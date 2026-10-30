#pragma once
#include <stdint.h>
void StickMarginInvalidate();
void StickMarginService();
void StickMarginDraw(bool preflight = false);
uint64_t StickMarginNextDeadline();
