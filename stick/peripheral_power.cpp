#include "peripheral_power.h"

#include "board_hal.h"
#include <Arduino.h>
#include <esp_timer.h>

namespace {
uint8_t users = 1;  // Board startup initially powers the LCD.
bool powered = true;
unsigned generation = 0;
int64_t retry_us = 0;

bool apply_power() {
  const bool wanted = users != 0;
  if (wanted == powered) { retry_us = 0; return true; }
  if (!StickBoardPeripheralSupply(wanted)) {
    retry_us = esp_timer_get_time() + 1000000;
    return false;
  }
  powered = wanted;
  if (powered) delay(2);
  else ++generation;  // Controller RAM/registers are no longer retained.
  retry_us = 0;
  return true;
}
}  // namespace

void StickPeripheralPowerInit(void) {
  users = 1;
  powered = true;
  generation = 0;
  retry_us = 0;
}

bool StickPeripheralAcquire(StickPeripheral owner) {
  users |= uint8_t(owner);
  return apply_power();
}

void StickPeripheralRelease(StickPeripheral owner) {
  users &= ~uint8_t(owner);
  apply_power();
}

void StickPeripheralPowerService(void) {
  if (retry_us && esp_timer_get_time() >= retry_us) apply_power();
}

unsigned StickPeripheralGeneration(void) { return generation; }
