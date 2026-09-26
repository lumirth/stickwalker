#include "display_panel.h"

#include "board_hal.h"
#include "display_bus.h"
#include "backlight.h"
#include "peripheral_power.h"
#include "display_palette.h"

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
constexpr unsigned kPanelHeight = kNativeHeight * kScale;
// Retain Dark as the default until the user saves an appearance choice.
bool dark_appearance = true;

uint8_t native_pixels[kNativeWidth * kNativeHeight];
uint8_t presented_pixels[kNativeWidth * kNativeHeight];
uint16_t *panel_pixels = nullptr;
bool backlight_on = true;
uint8_t backlight_level = 68;
bool panel_awake = true;
bool image_valid = false;
unsigned panel_generation = 0;
unsigned orientation = 3;
int64_t can_sleep_us = 0;
unsigned wake_stage = 0;
int64_t wake_deadline_us = 0;
int64_t prewarm_until_us = 0;

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
  screen->setRotation(3);
  screen->setBrightness(backlight_level);
  screen->fillScreen(StickDisplayBackground());
  panel_generation = StickPeripheralGeneration();
  panel_awake = true;
  image_valid = false;
  StickDisplayBusInit();
  if (!panel_pixels) {
    panel_pixels = static_cast<uint16_t *>(heap_caps_malloc(
        sizeof(uint16_t) * kPanelWidth * kPanelHeight,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  return panel_pixels != nullptr;
}

extern "C" void StickDisplayPanelSetOrientation(unsigned right_side_down) {
  auto *screen = StickBoardScreen();
  if (!screen) return;
  orientation = right_side_down ? 1 : 3;
  if (panel_awake) {
    screen->setRotation(orientation);
    screen->fillScreen(StickDisplayBackground());
  }
  image_valid = false;
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
  if (!StickPeripheralAcquire(StickPeripheral::Display)) return;
  if (panel_generation != StickPeripheralGeneration()) {
    StickBoardDisplayReset(false);
    wake_stage = 1;
    wake_deadline_us = esp_timer_get_time() + 8000;
  } else {
    screen->getPanel()->initBus();
    StickBacklightInit(0);
    screen->wakeup();
    wake_stage = 3;
    wake_deadline_us = esp_timer_get_time() + 130000;
  }
}

extern "C" void StickDisplayPowerService(void) {
  auto *screen = StickBoardScreen();
  if (!screen) return;
  const int64_t now = esp_timer_get_time();
  if (wake_stage && now >= wake_deadline_us) {
    if (wake_stage == 1) {
      StickBoardDisplayReset(true);
      wake_stage = 2;
      wake_deadline_us = now + 64000;
    } else if (wake_stage == 2) {
      if (!StickBoardDisplayInitRegisters()) {
        wake_stage = 0;
        StickPeripheralRelease(StickPeripheral::Display);
        return;
      }
      screen->wakeup();
      wake_stage = 3;
      wake_deadline_us = esp_timer_get_time() + 130000;
    } else {
      // writeCommand forwards bytes without selecting the panel. Match the
      // driver's sleep/wakeup transactions so these commands reach the LCD.
      screen->startWrite();
      screen->writeCommand(0x38);  // IDMOFF
      screen->writeCommand(0x29);  // DISPON
      screen->endWrite();
      if (panel_generation != StickPeripheralGeneration()) {
        screen->setSwapBytes(true);
        screen->invertDisplay(screen->getPanel()->getInvert());
        screen->setColorDepth(16);
        screen->setRotation(orientation);
        screen->fillScreen(StickDisplayBackground());
        panel_generation = StickPeripheralGeneration();
        image_valid = false;
      }
      wake_stage = 0;
      panel_awake = true;
      screen->setBrightness(backlight_on ? backlight_level : 0);
    }
  }
  if (!wake_stage && !backlight_on && panel_awake &&
      now >= can_sleep_us && now >= prewarm_until_us) {
    screen->sleep();
    delay(5);
    screen->getPanel()->releaseBus();
    StickBacklightSuspend();
    panel_awake = false;
    StickPeripheralRelease(StickPeripheral::Display);
  }
}

extern "C" int StickDisplayPanelIsReady(void) { return panel_awake; }

extern "C" uint64_t StickDisplayNextDeadline(void) {
  if (wake_stage) return uint64_t(wake_deadline_us);
  if (!backlight_on && panel_awake) {
    const int64_t deadline = can_sleep_us > prewarm_until_us ?
                             can_sleep_us : prewarm_until_us;
    return uint64_t(deadline);
  }
  return UINT64_MAX;
}

extern "C" void StickDisplayInvalidate(void) { image_valid = false; }

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
  image_valid = false;
  if (panel_awake) StickBoardScreen()->fillScreen(StickDisplayBackground());
  return 1;
}

extern "C" void StickDisplayPanelSetContrastDelta(unsigned delta) {
  auto *screen = StickBoardScreen();
  if (delta > 9) delta = 9;
  // Keep the source UI and save field. Its contrast step now sets the
  // transmissive panel's backlight, while pixel shades stay fixed.
  backlight_level = uint8_t(24 + 11 * delta);
  if (screen && backlight_on) screen->setBrightness(backlight_level);
}

extern "C" int StickDisplayPresent(void) {
  if (!panel_pixels) return 0;
  if (!StickDisplayIsPowered()) {
    StickDisplayPanelSetBacklight(0);
    return 1;
  }
  StickDisplayPanelSetBacklight(1);
  if (!panel_awake) return 0;
  StickDisplayFrame(native_pixels, sizeof(native_pixels));
  unsigned x0 = kNativeWidth, y0 = kNativeHeight, x1 = 0, y1 = 0;
  for (unsigned y = 0; y < kNativeHeight; ++y)
    for (unsigned x = 0; x < kNativeWidth; ++x)
      if (!image_valid || native_pixels[y * kNativeWidth + x] !=
                          presented_pixels[y * kNativeWidth + x]) {
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x + 1 > x1) x1 = x + 1;
        if (y + 1 > y1) y1 = y + 1;
      }
  if (x0 == kNativeWidth) return 1;
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
  std::memcpy(presented_pixels, native_pixels, sizeof(native_pixels));
  image_valid = true;
  return 1;
}
