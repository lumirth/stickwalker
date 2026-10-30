#include "display_panel.h"

#include "board_hal.h"
#include "display_bus.h"
#include "backlight.h"
#include "peripheral_power.h"
#include "display_palette.h"
#include "margin_status.h"
#include "control_settings.h"

#include <M5Unified.h>
#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <cstring>

namespace {

constexpr unsigned kNativeWidth = 96;
constexpr unsigned kNativeHeight = 64;
constexpr unsigned kScale = 2;
constexpr unsigned kPanelWidth = kNativeWidth * kScale;
constexpr unsigned kTransferRows = 8;
bool dark_appearance = true;

uint8_t presented_pixels[kNativeWidth * kNativeHeight];
uint16_t *panel_pixels = nullptr;
bool backlight_on = true;
uint8_t backlight_level = 68;
bool panel_awake = true;
uint64_t valid_rows = 0;
bool presentation_pending = true;
unsigned panel_generation = 0;
unsigned orientation = pw_stick::panel_rotation(unsigned(pw_stick::Orientation::LeftSideDown));
unsigned wake_stage = 0;
int64_t wake_deadline_us = 0;
int64_t wake_retry_us = 0;
int64_t prewarm_until_us = 0;
bool reset_required = false;

}  // namespace

extern "C" int StickDisplayPanelInit(void) {
  auto *screen = StickBoardScreen();
  if (!screen) return 0;
  Preferences preferences;
  if (preferences.begin("pw-display", true)) {
    dark_appearance = preferences.getUChar("dark", 1) != 0;
    preferences.end();
  }
  // The CPU stores uint16_t pixels little-endian. LGFX otherwise treats a
  // pushImage(uint16_t*) buffer as pre-swapped RGB565, which turns neutral
  // grays into colored pixels while black and white appear unchanged.
  screen->setSwapBytes(true);
  screen->setRotation(orientation);
  screen->setBrightness(backlight_level);
  screen->fillScreen(StickDisplayBackground());
  panel_generation = StickPeripheralGeneration();
  panel_awake = true;
  StickDisplayInvalidate();
  StickDisplayBusInit();
  if (!panel_pixels) {
    panel_pixels = static_cast<uint16_t *>(heap_caps_malloc(
        sizeof(uint16_t) * kPanelWidth * kTransferRows * kScale,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  return panel_pixels != nullptr;
}

extern "C" void StickDisplayPanelSetOrientation(unsigned setting) {
  auto *screen = StickBoardScreen();
  if (!screen) return;
  orientation = pw_stick::panel_rotation(setting);
  if (panel_awake) {
    screen->setRotation(orientation);
    screen->fillScreen(StickDisplayBackground());
  }
  StickDisplayInvalidate();
}

extern "C" void StickDisplayPanelSetBacklight(unsigned enabled) {
  auto *screen = StickBoardScreen();
  if (!screen) return;
  if (!enabled) {
    if (backlight_on) screen->setBrightness(0);
    backlight_on = false;
    StickDisplayPowerService();
    return;
  }
  backlight_on = true;
  if (panel_awake) { screen->setBrightness(backlight_level); return; }
  StickDisplayPrepareWake();
}

extern "C" void StickDisplayPrepareWake(void) {
  auto *screen = StickBoardScreen();
  if (!screen) return;
  prewarm_until_us = esp_timer_get_time() + 750000;
  if (panel_awake || wake_stage) return;
  if (esp_timer_get_time() < wake_retry_us) return;
  if (!StickPeripheralAcquire(StickPeripheral::Display)) {
    // Acquire records the supply request before writing the PMIC. Release
    // that request if panel preparation fails.
    StickPeripheralRelease(StickPeripheral::Display);
    wake_retry_us = esp_timer_get_time() + 1000000;
    return;
  }
  wake_retry_us = 0;
  if (reset_required || panel_generation != StickPeripheralGeneration()) {
    reset_required = true;
    StickBoardDisplayReset(false);
    wake_stage = 1;
    wake_deadline_us = esp_timer_get_time() + 8000;
  } else {
    screen->getPanel()->initBus();
    if (!StickBacklightInit(0)) {
      screen->getPanel()->releaseBus();
      StickBacklightSuspend();
      StickPeripheralRelease(StickPeripheral::Display);
      wake_retry_us = esp_timer_get_time() + 1000000;
      return;
    }
    screen->wakeup();
    wake_stage = 3;
    wake_deadline_us = esp_timer_get_time() + 130000;
  }
}

extern "C" void StickDisplayCancelPreparedWake(void) {
  if (backlight_on) return;
  prewarm_until_us = 0;
  wake_retry_us = 0;
  if (wake_stage == 1 || wake_stage == 2) {
    // Reset preparation has not issued Sleep-out. Abort now, and remember
    // that reset lost controller state even if audio keeps the rail powered.
    StickBoardDisplayReset(false);
    reset_required = true;
    wake_stage = 0;
    StickBoardScreen()->getPanel()->releaseBus();
    StickBacklightSuspend();
    panel_awake = false;
    StickPeripheralRelease(StickPeripheral::Display);
    return;
  }
  // Finish Sleep-out's 130 ms wait before sending Sleep-in. Releasing the
  // preparation request skips Display-on.
  StickDisplayPowerService();
}

extern "C" void StickDisplayPowerService(void) {
  auto *screen = StickBoardScreen();
  if (!screen) return;
  const int64_t now = esp_timer_get_time();
  if (!panel_awake && !wake_stage && wake_retry_us &&
      now >= wake_retry_us) {
    if (backlight_on || now < prewarm_until_us) StickDisplayPrepareWake();
    else wake_retry_us = 0;
  }
  if (wake_stage && now >= wake_deadline_us) {
    if (wake_stage == 1) {
      StickBoardDisplayReset(true);
      wake_stage = 2;
      wake_deadline_us = now + 64000;
    } else if (wake_stage == 2) {
      if (!StickBoardDisplayInitRegisters()) {
        wake_stage = 0;
        screen->getPanel()->releaseBus();
        StickBacklightSuspend();
        StickPeripheralRelease(StickPeripheral::Display);
        wake_retry_us = esp_timer_get_time() + 1000000;
        return;
      }
      screen->wakeup();
      wake_stage = 3;
      wake_deadline_us = esp_timer_get_time() + 130000;
    } else if (wake_stage == 3) {
      if (!backlight_on && now >= prewarm_until_us) {
        wake_stage = 0;
        panel_awake = true;  // Sleep-out completed; Sleep-in is now legal.
      } else {
        // writeCommand forwards bytes without selecting the panel. Match the
        // driver's sleep/wakeup transactions so these commands reach the LCD.
        screen->startWrite();
        screen->writeCommand(0x38);  // IDMOFF
        screen->writeCommand(0x29);  // DISPON
        screen->endWrite();
        screen->setRotation(orientation);
        if (reset_required || panel_generation != StickPeripheralGeneration()) {
          screen->setSwapBytes(true);
          screen->invertDisplay(screen->getPanel()->getInvert());
          screen->setColorDepth(16);
          screen->fillScreen(StickDisplayBackground());
          panel_generation = StickPeripheralGeneration();
          reset_required = false;
          StickDisplayInvalidate();
        }
        wake_stage = 0;
        panel_awake = true;
        screen->setBrightness(backlight_on ? backlight_level : 0);
      }
    } else {
      screen->getPanel()->releaseBus();
      StickBacklightSuspend();
      wake_stage = 0;
      StickPeripheralRelease(StickPeripheral::Display);
      if (backlight_on || now < prewarm_until_us) StickDisplayPrepareWake();
    }
  }
  if (!wake_stage && !backlight_on && panel_awake &&
      now >= prewarm_until_us) {
    screen->sleep();
    panel_awake = false;
    wake_stage = 4;
    wake_deadline_us = esp_timer_get_time() + 5000;
  }
}

extern "C" int StickDisplayPanelIsReady(void) { return panel_awake; }

extern "C" uint64_t StickDisplayNextDeadline(void) {
  if (wake_stage) return uint64_t(wake_deadline_us);
  if (!backlight_on && panel_awake) return uint64_t(prewarm_until_us);
  if (wake_retry_us && (backlight_on ||
      esp_timer_get_time() < prewarm_until_us)) return uint64_t(wake_retry_us);
  return UINT64_MAX;
}

extern "C" void StickDisplayInvalidate(void) {
  valid_rows = 0;
  presentation_pending = true;
  StickMarginInvalidate();
}

extern "C" int StickDisplayPresentationPending(void) {
  return presentation_pending;
}

extern "C" unsigned StickDisplayIsDark(void) { return dark_appearance; }
extern "C" uint16_t StickDisplayBackground(void) {
  return pw_stick::palette_color(dark_appearance, 0);
}
extern "C" uint16_t StickDisplayForeground(void) {
  return pw_stick::palette_color(dark_appearance, 3);
}
extern "C" int StickDisplaySetDark(unsigned dark) {
  if (dark > 1) return 0;
  if (dark_appearance == bool(dark)) return 1;
  Preferences preferences;
  if (!preferences.begin("pw-display", false)) return 0;
  const size_t written = preferences.putUChar("dark", dark);
  preferences.end();
  if (written != 1) return 0;
  dark_appearance = dark;
  StickDisplayInvalidate();
  if (panel_awake) StickBoardScreen()->fillScreen(StickDisplayBackground());
  return 1;
}

extern "C" void StickDisplayPanelSetContrastDelta(unsigned delta) {
  auto *screen = StickBoardScreen();
  if (delta > 9) delta = 9;
  // Map the game's Contrast setting to backlight brightness.
  backlight_level = uint8_t(24 + 11 * delta);
  if (screen && backlight_on && panel_awake)
    screen->setBrightness(backlight_level);
}

extern "C" int StickDisplayPresent(const StickDisplaySnapshot *snapshot) {
  if (!panel_pixels || !snapshot) return 0;
  if (!snapshot->powered) {
    StickDisplayPanelSetBacklight(0);
    presentation_pending = false;
    return 1;
  }
  StickDisplayPanelSetBacklight(1);
  presentation_pending = true;
  if (!panel_awake) return 0;
  const uint8_t *native_pixels = snapshot->pixels;
  unsigned x0 = kNativeWidth, y0 = kNativeHeight, x1 = 0, y1 = 0;
  for (unsigned y = 0; y < kNativeHeight; ++y)
    for (unsigned x = 0; x < kNativeWidth; ++x)
      if (!(valid_rows & (UINT64_C(1) << y)) ||
          native_pixels[y * kNativeWidth + x] !=
              presented_pixels[y * kNativeWidth + x]) {
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x + 1 > x1) x1 = x + 1;
        if (y + 1 > y1) y1 = y + 1;
      }
  if (x0 == kNativeWidth) {
    presentation_pending = false;
    return 1;
  }
  const unsigned end = y1;
  if (y1 > y0 + kTransferRows) y1 = y0 + kTransferRows;
  const unsigned width = (x1 - x0) * kScale;
  for (unsigned y = y0; y < y1; ++y) {
    for (unsigned x = x0; x < x1; ++x) {
      const uint16_t color = pw_stick::palette_color(
          dark_appearance, native_pixels[y * kNativeWidth + x]);
      const unsigned offset = (y - y0) * width * kScale + (x - x0) * kScale;
      panel_pixels[offset] = panel_pixels[offset + 1] = color;
      panel_pixels[offset + width] = panel_pixels[offset + width + 1] = color;
    }
  }
  StickBoardScreen()->pushImage(24 + x0 * kScale, 3 + y0 * kScale,
                               width, (y1 - y0) * kScale, panel_pixels);
  for (unsigned y = y0; y < y1; ++y) {
    std::memcpy(presented_pixels + y * kNativeWidth + x0,
                native_pixels + y * kNativeWidth + x0, x1 - x0);
    valid_rows |= UINT64_C(1) << y;
  }
  presentation_pending = y1 < end;
  return !presentation_pending;
}
