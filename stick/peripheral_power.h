#pragma once

#include <stdint.h>

enum class StickPeripheral : uint8_t { Display = 1, Sound = 2 };

// Coordinate LCD, codec and microphone supply requests on the application
// task. Switch off the supply after its last user releases it.
void StickPeripheralPowerInit(void);
bool StickPeripheralAcquire(StickPeripheral owner);
void StickPeripheralRelease(StickPeripheral owner);
void StickPeripheralPowerService(void);
unsigned StickPeripheralGeneration(void);
uint64_t StickPeripheralNextDeadline(void);
