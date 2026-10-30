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
#include "diode_status.h"
#include "diode_feedback.h"
#include "support/ir.h"
#include <M5Unified.h>
#include <esp_timer.h>

namespace {
bool opened = false;
bool draw_pending = false;
bool details = false;
unsigned row = 0;
uint64_t last_input = 0;
uint64_t last_battery = 0;
constexpr uint64_t kIdleUs = 90000000;

const char *connection_outcome() {
  const auto &attempt = *StickLastConnection();
  if (!attempt.started) return "No connection yet";
  if (!attempt.finished) return "Connecting";
  switch (attempt.result) {
  case IR_RESULT_NONE: return "Completed";
  case IR_RESULT_NO_RESPONSE: return "No reply";
  case IR_RESULT_REJECTED: return "Connection rejected";
  case IR_RESULT_NO_POKEMON: return "No Pokemon to exchange";
  case IR_RESULT_PEER_ALREADY_SEEN: return "Already met this Walker";
  case IR_RESULT_EVENT_ALREADY_RECEIVED: return "Already received";
  case IR_RESULT_EVENT_NOT_RECEIVED: return "Event not received";
  default: return "Connection interrupted";
  }
}
const char *connection_probe_summary() {
  const auto &attempt = *StickLastConnection();
  if (!attempt.checked) return "Diode: not checked";
  if (!attempt.receive_safe) return attempt.preflight == unsigned(pw_stick::DiodeState::Reversed)
      ? "Diode: appears reversed" : "Receiver input blocked";
  if (attempt.preflight == unsigned(pw_stick::DiodeState::Missing)) return "Diode: not detected";
  if (attempt.preflight == unsigned(pw_stick::DiodeState::Connected)) return "Diode: detected";
  return "Diode: check inconclusive";
}
const char *connection_traffic_summary() {
  const auto &attempt = *StickLastConnection();
  return attempt.packets ? "Valid packet received" : "No valid packet received";
}

void battery() {
  if (!StickDisplayPanelIsReady()) return;
  auto *screen = StickBoardScreen();
  const unsigned millivolts = StickBatteryMillivolts();
  const int percent = StickBatteryPercentEstimate(millivolts);
  screen->fillRect(8, 27, 224, 12, StickDisplayBackground());
  screen->setTextColor(StickDisplayForeground(), StickDisplayBackground());
  screen->setTextSize(1);
  screen->setCursor(10, 28);
  uint16_t vbus;
  const bool usb = StickBoardVbusVoltage(&vbus) && vbus >= 4000;
  if (percent < 0) screen->print("Battery reading unavailable");
  else screen->printf("Battery %u.%02u V  ~%d%%  %s", millivolts / 1000,
      (millivolts % 1000) / 10, percent,
      usb ? "USB" : "");
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
  screen->print(details ? "LAST CONNECTION" : "STICK SETTINGS");
  if (details) {
    screen->setTextSize(1);
    screen->setCursor(10, 32);
    screen->print(connection_outcome());
    const auto &attempt = *StickLastConnection();
    if (attempt.started) {
      screen->setCursor(10, 50);
      screen->print(connection_probe_summary());
      screen->setCursor(10, 65);
      screen->print(!attempt.checked ? "Receiver: not checked" :
                    attempt.receive_safe ? "Receiver: able to start" : "Receiver: unable to start");
      screen->setCursor(10, 80);
      screen->print(connection_traffic_summary());
      screen->setCursor(10, 95);
      screen->printf("Packets: %u", attempt.packets);
    }
    screen->setCursor(10, 125);
    screen->print("L back");
    return;
  }
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
  screen->setCursor(10, 107);
  screen->printf("%c Last connection", row == 5 ? '>' : ' ');
  screen->setCursor(10, 125);
  screen->print("M next  R change  L close");
  StickNativeService();
}

void close(bool wake) {
  opened = draw_pending = details = false;
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
    details = false;
    last_input = now;
    StickInputMenuMode(1);
    draw();
  }
  if (!opened) return;
  if (now - last_input >= kIdleUs) { close(false); return; }
  if (draw_pending) draw();
  if (!details && now - last_battery >= 5000000) battery();
  const u8 buttons = StickInputTakeMenuButtons();
  if (!buttons) return;
  last_input = now;
  if (details) {
    if (buttons & 4u) { details = false; draw(); }
    return;
  }
  if (buttons & 4u) { close(true); return; }
  if (buttons & 1u) {
    row = (row + 1) % 6;
    if (row == 3 && StickInputProfile()) ++row;
    draw();
    return;
  }
  if (!(buttons & 2u)) return;
  if (row == 5) { details = true; draw(); return; }
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
  const uint64_t battery_deadline = details ? UINT64_MAX : last_battery + 5000000;
  const uint64_t idle_deadline = last_input + kIdleUs;
  return battery_deadline < idle_deadline ? battery_deadline : idle_deadline;
}
