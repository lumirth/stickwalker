#pragma once

#include <stdint.h>

enum class StickPeripheral : uint8_t { Display = 1, Sound = 2 };

// Application-task ownership of the shared LCD/codec/microphone supply.
// Neither owner may switch off the other's hardware.
void StickPeripheralPowerInit(void);
bool StickPeripheralAcquire(StickPeripheral owner);
void StickPeripheralRelease(StickPeripheral owner);
void StickPeripheralPowerService(void);
unsigned StickPeripheralGeneration(void);
