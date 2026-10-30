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
bool game_visible = false;
bool power_baseline = true;
uint64_t next_battery = 0;
uint64_t battery_valid_at = 0;
uint64_t power_valid_at = 0;
uint64_t event_at = 0;
uint64_t visible_until = 0;
int battery = -1;
int event_battery = -1;
int power = -1;
int power_candidate = -1;
unsigned power_confirmations = 0;
unsigned low_confirmations = 0;
unsigned recovered_confirmations = 0;
bool low = false;
unsigned drawn_diode = 255;
int drawn_battery = -2;
int drawn_power = -2;

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
void StickMarginPausePowerEvents() {
  power_baseline = true;
  power_confirmations = 0;
  event_at = visible_until = 0;
}
void StickMarginObservePower(int plugged) {
  const uint64_t now = uint64_t(esp_timer_get_time());
  if (plugged < 0) {
    power_confirmations = 0;
    if (now - power_valid_at > 5000000) power = -1;
    return;
  }
  power_valid_at = now;
  const bool visible = StickDisplayIsPowered() && !StickSettingsIsOpen();
  if (power_baseline || !visible || power < 0) {
    power = plugged;
    power_baseline = false;
    power_confirmations = 0;
    return;
  }
  if (plugged != power_candidate) { power_candidate = plugged; power_confirmations = 0; }
  if (++power_confirmations < 2 || power == plugged) return;
  power = plugged;
  event_at = now;
  visible_until = 0;
  next_battery = now;
}
void StickMarginService() {
  if (StickForegroundIsIr()) return;
  const bool visible = StickDisplayIsPowered() && !StickSettingsIsOpen();
  if (visible != game_visible) {
    game_visible = visible;
    StickMarginPausePowerEvents();
    next_battery = 0;
    dirty = true;
  }
  if (!visible) return;
  const uint64_t now = uint64_t(esp_timer_get_time());
  if (battery >= 0 && now - battery_valid_at > 15000000) battery = -1;
  if (now < next_battery) return;
  const unsigned mv = StickBatteryMillivolts();
  const int percent = StickBatteryPercentEstimate(mv);
  if (percent >= 0) {
    battery_valid_at = now;
    battery = percent < 5 ? 0 : percent < 25 ? 1 : percent < 50 ? 2 : percent < 75 ? 3 : 4;
    low_confirmations = mv < 3340 ? low_confirmations + 1 : 0;
    recovered_confirmations = mv >= 3440 ? recovered_confirmations + 1 : 0;
    if (low_confirmations >= 2) low = true;
    if (recovered_confirmations >= 2) low = false;
  } else {
    low_confirmations = recovered_confirmations = 0;
  }
  next_battery = now + 5000000;
}
uint64_t StickMarginNextDeadline() {
  if (!StickDisplayIsPowered() || StickSettingsIsOpen()) return UINT64_MAX;
  const uint64_t now = uint64_t(esp_timer_get_time());
  return visible_until > now && visible_until < next_battery ? visible_until : next_battery;
}
void StickMarginDraw(bool preflight) {
  if ((!preflight && StickForegroundIsIr()) || StickSettingsIsOpen() ||
      !StickDisplayIsPowered() || !StickDisplayPanelIsReady()) return;
  const uint64_t now = uint64_t(esp_timer_get_time());
  if (event_at) {
    if (now - event_at <= 5000000) {
      visible_until = now + 2500000;
      event_battery = battery;
    }
    event_at = 0;
  }
  const bool transient = now < visible_until;
  const bool warning = low && power != 1;
  const int level = warning ? 0 : transient ? event_battery : -1;
  const int cable = transient && power == 1;
  const unsigned diode = StickDiodeVisible();
  if (!dirty && diode == drawn_diode && level == drawn_battery && cable == drawn_power) return;
  const unsigned x = pw_stick::diode_icon_x(StickInputOrientation());
  const unsigned y = pw_stick::diode_icon_y(StickInputOrientation());
  const uint16_t blank[16] = {};
  icon(x, y, diode ? pw_stick::kDiodeIcons[diode - 1] : blank);
  uint16_t rows[16] = {};
  if (level >= 0) {
    rows[4] = rows[11] = 0x3ffe;
    for (unsigned row = 5; row < 11; ++row) {
      rows[row] = 0x2002;
      if (row >= 6 && row <= 9) rows[row] |= 0xc000;
      for (int bar = 0; bar < level; ++bar) rows[row] |= uint16_t(3u << (3 + bar * 2));
    }
  }
  if (cable) { rows[0] = 0x0140; rows[1] = 0x03e0; rows[2] = 0x0080; }
  icon(x, 119 - y, rows);
  dirty = false;
  drawn_diode = diode;
  drawn_battery = level;
  drawn_power = cable;
}
