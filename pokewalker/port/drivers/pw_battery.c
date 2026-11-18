#include "project.h"
#include "pokewalker_host.h"
#include "application/pw_battery.h"
uint BatterySample(void)
{
  return pw_host_battery_low() ? 0 : 0x100;
}
u8 BatteryCheckLow(u16 scale)
{
  (void)scale;
  return pw_host_battery_low();
}
void BatteryUpdate(void)
{
  g_state.events.bits.batteryLow = pw_host_battery_low();
  g_state.events.bits.batteryCheckPending = 0;
}
