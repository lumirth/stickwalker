#include "board_settings.h"

#include "battery_bridge.h"
#include "board_hal.h"
#include "controls.h"
#include "control_settings.h"
#include "display_bus.h"
#include "display_palette.h"
#include "display_panel.h"
#include "foreground_bridge.h"
#include "input_bridge.h"
#include "native_runtime.h"
#include "sound_bridge.h"
#include "ir_transport.h"
#include <M5Unified.h>
#include <esp_timer.h>

namespace {
bool opened = false;
bool draw_pending = false;
unsigned row = 0;
uint64_t last_input = 0;
uint64_t last_battery = 0;
constexpr uint64_t kIdleUs = 90000000;

void battery() {
  if (!StickDisplayPanelIsReady()) return;
  auto *screen = StickBoardScreen();
  const unsigned millivolts = StickBatteryMillivolts();
  const int percent = StickBatteryPercentEstimate(millivolts);
  screen->fillRect(8, 27, 224, 12, StickDisplayBackground());
  screen->setTextColor(StickDisplayForeground(), StickDisplayBackground());
  screen->setTextSize(1);
  screen->setCursor(10, 28);
  if (percent < 0) screen->print("Battery reading unavailable");
  else screen->printf("Battery %u.%02u V  ~%d%%  %s", millivolts / 1000,
      (millivolts % 1000) / 10, percent,
      StickBoardPower().getVBUSVoltage() >= 4000 ? "USB" : "");
  screen->fillRect(8, 125, 224, 8, StickDisplayBackground());
  screen->setCursor(10, 125);
  screen->print(StickIrLastSummary());
  last_battery = uint64_t(esp_timer_get_time());
}

void draw() {
  StickDisplayPanelSetBacklight(1);
  draw_pending = !StickDisplayPanelIsReady();
  if (draw_pending) return;
  auto *screen = StickBoardScreen();
  screen->fillScreen(StickDisplayBackground());
  StickNativeService();
  screen->setTextColor(StickDisplayForeground(), StickDisplayBackground());
  screen->setTextSize(2);
  screen->setCursor(8, 8);
  screen->print("STICK SETTINGS");
  battery();
  screen->setTextSize(1);
  screen->setCursor(10, 42);
  screen->printf("%c Input: %s", row == 0 ? '>' : ' ',
      pw_stick::layout_name(pw_stick::Layout(StickInputLayout())));
  screen->setCursor(10, 55);
  screen->printf("%c Rotation: %s", row == 1 ? '>' : ' ',
      pw_stick::right_side_down(StickInputOrientation()) ? "Right side down" : "Left side down");
  screen->setCursor(10, 68);
  screen->printf("%c Appearance: %s", row == 2 ? '>' : ' ',
      StickDisplayIsDark() ? "Dark" : "Light");
  for (unsigned shade = 0; shade < 4; ++shade)
    screen->fillRect(176 + shade * 12, 68, 10, 8,
        pw_stick::palette_color(StickDisplayIsDark(), shade));
  screen->setCursor(10, 81);
  if (StickInputProfile()) screen->print("  Center: M");
  else screen->printf("%c M+R window: %u ms", row == 3 ? '>' : ' ',
      80 + StickInputChordWindowIndex() * 40);
  screen->setCursor(10, 94);
  screen->printf("%c Test speaker", row == 4 ? '>' : ' ');
  screen->drawFastHLine(8, 107, 224, StickDisplayForeground());
  screen->setCursor(10, 115);
  screen->print("M next  R change  L close");
  StickNativeService();
}

void close(bool wake) {
  opened = draw_pending = false;
  StickInputMenuMode(0);
  if (wake) StickForegroundDeviceMenuClosed();
  if (StickDisplayPanelIsReady()) StickBoardScreen()->fillScreen(StickDisplayBackground());
  StickDisplayInvalidate();
}
}

void StickSettingsService(void) {
  const uint64_t now = uint64_t(esp_timer_get_time());
  if (!opened && StickMenuRequested()) {
    opened = true;
    row = 0;
    last_input = now;
    StickInputMenuMode(1);
    draw();
  }
  if (!opened) return;
  if (now - last_input >= kIdleUs) { close(false); return; }
  if (draw_pending) draw();
  if (now - last_battery >= 5000000) battery();
  const u8 buttons = StickInputTakeMenuButtons();
  if (!buttons) return;
  last_input = now;
  if (buttons & 4u) { close(true); return; }
  if (buttons & 1u) {
    row = (row + 1) % 5;
    if (row == 3 && StickInputProfile()) ++row;
    draw();
    return;
  }
  if (!(buttons & 2u)) return;
  if (row == 4) { StickSoundTestTone(); return; }
  if (row == 2) {
    if (StickDisplaySetDark(!StickDisplayIsDark())) draw();
    return;
  }
  const u8 layout = StickInputLayout();
  const u8 orientation = StickInputOrientation();
  const u8 chord = StickInputChordWindowIndex();
  const u8 next_orientation = row == 1 ? orientation ^ 1u : orientation;
  if (StickInputConfigure(row == 0 ? (layout + 1u) % 4u : layout,
      next_orientation, row == 3 ? (chord + 1u) % 3u : chord)) {
    if (next_orientation != orientation)
      StickDisplayPanelSetOrientation(next_orientation);
    draw();
  }
}

int StickSettingsIsOpen(void) { return opened; }
unsigned StickSettingsSelection(void) { return row; }
uint64_t StickSettingsNextDeadline(void) {
  if (!opened) return UINT64_MAX;
  const uint64_t battery_deadline = last_battery + 5000000;
  const uint64_t idle_deadline = last_input + kIdleUs;
  return battery_deadline < idle_deadline ? battery_deadline : idle_deadline;
}
