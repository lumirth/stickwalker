#include "margin_status.h"
#include "battery_bridge.h"
#include "board_hal.h"
#include "board_settings.h"
#include "diode_feedback.h"
#include "diode_status.h"
#include "display_bus.h"
#include "display_panel.h"
#include "foreground_bridge.h"
#include "input_bridge.h"
#include <esp_timer.h>

namespace {
bool dirty = true;
uint64_t next_battery = 0;
int battery = -1;
bool usb = false;
unsigned drawn_diode = 255;

void icon(unsigned x, unsigned y, const uint16_t *rows) {
  uint16_t pixels[256];
  for (unsigned row = 0; row < 16; ++row)
    for (unsigned col = 0; col < 16; ++col)
      pixels[row * 16 + col] = rows[row] & (1u << col)
          ? StickDisplayForeground() : StickDisplayBackground();
  StickBoardScreen()->pushImage(x, y, 16, 16, pixels);
}
}

void StickMarginInvalidate() { dirty = true; }
void StickMarginService() {
  if (StickForegroundIsIr() || StickSettingsIsOpen() || !StickDisplayIsPowered()) return;
  const uint64_t now = uint64_t(esp_timer_get_time());
  if (now < next_battery) return;
  const int percent = StickBatteryPercentEstimate(StickBatteryMillivolts());
  const int level = percent < 0 ? -1 : percent < 5 ? 0 : percent < 25 ? 1 :
      percent < 50 ? 2 : percent < 75 ? 3 : 4;
  uint16_t vbus;
  const bool plugged = StickBoardVbusVoltage(&vbus) && vbus >= 4000;
  dirty |= level != battery || plugged != usb;
  battery = level;
  usb = plugged;
  next_battery = now + 5000000;
}
uint64_t StickMarginNextDeadline() {
  return StickDisplayIsPowered() && !StickSettingsIsOpen() ? next_battery : UINT64_MAX;
}
void StickMarginDraw(bool preflight) {
  if ((!preflight && StickForegroundIsIr()) || StickSettingsIsOpen() ||
      !StickDisplayIsPowered() || !StickDisplayPanelIsReady()) return;
  const unsigned diode = StickDiodeVisible();
  if (!dirty && diode == drawn_diode) return;
  const unsigned x = pw_stick::diode_icon_x(StickInputOrientation());
  const unsigned y = pw_stick::diode_icon_y(StickInputOrientation());
  const uint16_t blank[16] = {};
  icon(x, y, diode ? pw_stick::kDiodeIcons[diode - 1] : blank);
  uint16_t rows[16] = {};
  rows[4] = rows[11] = 0x3ffe;
  for (unsigned row = 5; row < 11; ++row) {
    rows[row] = 0x2002;
    if (row >= 6 && row <= 9) rows[row] |= 0xc000;
    for (int bar = 0; bar < battery; ++bar) rows[row] |= uint16_t(3u << (3 + bar * 2));
  }
  if (battery < 0) {
    rows[6] |= 0x0380; rows[7] |= 0x0200; rows[8] |= 0x0100; rows[10] |= 0x0100;
  }
  if (usb) { rows[0] = 0x0140; rows[1] = 0x03e0; rows[2] = 0x0080; }
  icon(x, 119 - y, rows);
  dirty = false;
  drawn_diode = diode;
}
