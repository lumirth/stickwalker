#include "board_runtime.h"
#include "board_diagnostics.h"
#include "board_settings.h"
#include "board_hal.h"
#include "display_bus.h"
#include "display_panel.h"
#include "eeprom_backend.h"
#include "foreground_bridge.h"
#include "input_bridge.h"
#include "ir_transport.h"
#include "native_runtime.h"
#include "sound_bridge.h"
#include "power_sleep.h"
#include "backlight.h"
#include "peripheral_power.h"
#include "diode_status.h"
#include "margin_status.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <esp_timer.h>

extern "C" void StickPortBoot(void);

namespace {
bool ready = false;
bool usb_present = true;
bool previous_ir = false;
uint64_t next_vbus_check = 0;
uint64_t next_input = 0;
uint64_t presented_revision = UINT64_MAX;
StickDisplaySnapshot snapshot;

void fatal(const char *reason) {
  Serial.printf("PW_STICK_FATAL %s\n", reason);
  if (auto *screen = StickBoardScreen()) {
    screen->fillScreen(TFT_BLACK);
    screen->setTextColor(TFT_WHITE, TFT_BLACK);
    screen->setCursor(8, 12);
    screen->print(reason);
  }
}

void refresh_usb() {
  uint16_t millivolts;
  const bool valid = StickBoardVbusVoltage(&millivolts);
  usb_present = !valid || millivolts >= 4000;
  StickMarginObservePower(valid ? int(millivolts >= 4000) : -1);
  next_vbus_check = uint64_t(esp_timer_get_time()) + 1000000;
}

uint64_t next_deadline() {
  uint64_t deadline = StickNativeNextDeadline();
  for (const uint64_t candidate : {next_input, next_vbus_check,
       StickDisplayNextDeadline(), StickSoundNextDeadline(),
       StickPeripheralNextDeadline(), StickSleepNextDeadline(),
       StickSettingsNextDeadline(), StickDiodeNextDeadline(), StickMarginNextDeadline()})
    if (candidate < deadline) deadline = candidate;
  return deadline;
}
}

void StickPresentGame(int force) {
  if (StickSettingsIsOpen()) return;
  StickMarginDraw();
  if (force) StickDisplayInvalidate();
  const uint64_t revision = StickDisplayRevision();
  if (revision == presented_revision && !StickDisplayPresentationPending()) return;
  StickDisplayCapture(&snapshot);
  const uint64_t started = uint64_t(esp_timer_get_time());
  if (StickDisplayPresent(&snapshot)) presented_revision = snapshot.revision;
  StickDiagnosticsPresent(uint64_t(esp_timer_get_time()) - started);
  StickNativeService();
}

extern "C" void StickPlatformPrepareIr(void) {
  StickMarginPausePowerEvents();
  StickInputRequireRelease();
  StickSoundQuiesceForIr();
  StickPresentGame(0);
  while (!StickSettingsIsOpen() && StickDisplayPanelIsReady() &&
         StickDisplayPresentationPending()) StickPresentGame(0);
}

extern "C" void StickPlatformPresentIr(void) { StickPresentGame(0); }

extern "C" void StickPortSetup(void) {
  Serial.begin(115200);
  setCpuFrequencyMhz(240);
  if (!StickBoardBegin()) { fatal("Board startup failed"); return; }
  if (!StickDisplayPanelInit()) { fatal("Display startup failed"); return; }
  if (!StickEepromMount()) { fatal("Game storage unavailable"); return; }
  StickNativeInit();
  StickEepromDefer(1);
  StickPortBoot();
  StickEepromDefer(0);
  if (!StickEepromHealthy()) { fatal("Game storage checkpoint failed"); return; }
  StickDisplayPanelSetOrientation(StickInputOrientation());
  StickSoundPrepare();
  StickPresentGame(1);
  StickBoardPower().setExtOutput(false);
  if (!StickSleepBegin()) Serial.printf("PW_STICK_SLEEP_SETUP_FAILED retry_ms=1000\n");
  refresh_usb();
  next_input = uint64_t(esp_timer_get_time());
  if (!setCpuFrequencyMhz(80)) Serial.printf("PW_STICK_IDLE_CLOCK_FAILED\n");
  StickDiagnosticsInit();
  StickNativeStart();
  ready = true;
}

extern "C" void StickPortLoop(void) {
  if (!ready) { delay(50); return; }
  if (StickDiagnosticsService()) return;
  StickNativeService();
  if (!StickForegroundIsIr()) StickIrServiceDiagnostics();
  if (StickForegroundIsIr()) {
    previous_ir = true;
    StickNativeRun();
    // The native session owns receive/service turns. Polling drains raw capture
    // while the sampler runs; a scheduler wait here can lose the next request.
    return;
  }
  if (previous_ir) {
    previous_ir = false;
    next_input = uint64_t(esp_timer_get_time());
    if (StickDisplayIsPowered()) StickSoundPrepare();
  }

  uint64_t now = uint64_t(esp_timer_get_time());
  if (next_input == UINT64_MAX && (usb_present || StickSoundIsBusy()))
    next_input = now;
  if (now >= next_input) {
    StickInputPoll(millis());
    StickDiagnosticsInput(uint64_t(esp_timer_get_time()) - now);
    next_input = StickInputNextDeadline();
  }
  StickNativeService();
  StickSettingsService();
  now = uint64_t(esp_timer_get_time());
  if (StickDisplayIsPowered() || StickSettingsIsOpen()) StickSoundPrepare();
  else if (!StickInputWakeScanActive()) StickSoundSuspend();
  StickSoundService();
  StickDiagnosticsSound(uint64_t(esp_timer_get_time()) - now);
  StickDisplayPowerService();
  StickPeripheralPowerService();
  StickDiodeService();
  StickMarginService();

  now = uint64_t(esp_timer_get_time());
  const unsigned task = StickNativeRun();
  StickDiagnosticsForeground(task, uint64_t(esp_timer_get_time()) - now);
  if (StickForegroundIsIr()) return;
  StickPresentGame(0);
  now = uint64_t(esp_timer_get_time());
  if (now >= next_vbus_check) refresh_usb();
  const uint64_t deadline = next_deadline();
  if (!usb_present && !StickSoundIsBusy() &&
      ((!StickDisplayIsPowered() && !StickSettingsIsOpen()) || StickBacklightSleepReady())) {
    StickDiagnosticsBeforeSleep(usb_present);
    if (StickSleepUntil(deadline)) {
      next_input = uint64_t(esp_timer_get_time());
      return;
    }
    if (next_input == UINT64_MAX) next_input = now + 5000;
  }
  if (deadline > now + 1000) delay(1);
}
