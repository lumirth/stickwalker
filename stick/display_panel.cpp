#include "display_panel.h"

#include "board_hal.h"
#include "display_bus.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>

namespace {

constexpr unsigned kNativeWidth = 96;
constexpr unsigned kNativeHeight = 64;
constexpr unsigned kScale = 2;
constexpr unsigned kPanelWidth = kNativeWidth * kScale;
constexpr unsigned kPanelHeight = kNativeHeight * kScale;
// Show the original four intensity levels as light pixels on a dark panel.
constexpr uint16_t kPalette[4] = {0x0000, 0x52aa, 0xad55, 0xffff};

uint8_t native_pixels[kNativeWidth * kNativeHeight];
uint16_t *panel_pixels = nullptr;
bool backlight_on = true;
uint8_t backlight_level = 68;

}  // namespace

extern "C" int StickDisplayPanelInit(void) {
  auto *screen = StickBoardScreen();
  if (!screen) return 0;
  // The CPU stores uint16_t pixels little-endian. LGFX otherwise treats a
  // pushImage(uint16_t*) buffer as pre-swapped RGB565, which turns neutral
  // grays into colored pixels while black and white appear unchanged.
  screen->setSwapBytes(true);
  screen->setRotation(3);
  screen->setBrightness(backlight_level);
  screen->fillScreen(kPalette[0]);
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
  screen->setRotation(right_side_down ? 1 : 3);
  screen->fillScreen(kPalette[0]);
}

extern "C" void StickDisplayPanelSetBacklight(unsigned enabled) {
  auto *screen = StickBoardScreen();
  if (!screen || backlight_on == (enabled != 0)) return;
  backlight_on = enabled != 0;
  screen->setBrightness(backlight_on ? backlight_level : 0);
}

extern "C" void StickDisplayPanelSetContrastDelta(unsigned delta) {
  auto *screen = StickBoardScreen();
  if (delta > 9) delta = 9;
  // Keep the source UI and save field. Its contrast step now sets the
  // transmissive panel's backlight, while pixel shades stay fixed.
  backlight_level = uint8_t(24 + 11 * delta);
  if (screen && backlight_on) screen->setBrightness(backlight_level);
}

extern "C" void StickDisplayPresent(void) {
  if (!panel_pixels) return;
  if (!StickDisplayIsPowered()) {
    StickDisplayPanelSetBacklight(0);
    return;
  }
  StickDisplayPanelSetBacklight(1);
  StickDisplayFrame(native_pixels, sizeof(native_pixels));
  for (unsigned y = 0; y < kNativeHeight; ++y) {
    for (unsigned x = 0; x < kNativeWidth; ++x) {
      const uint16_t color = kPalette[native_pixels[y * kNativeWidth + x] & 3u];
      const unsigned offset = y * (kPanelWidth * kScale) + x * kScale;
      panel_pixels[offset] = panel_pixels[offset + 1] = color;
      panel_pixels[offset + kPanelWidth] =
          panel_pixels[offset + kPanelWidth + 1] = color;
    }
  }
  StickBoardScreen()->pushImage(24, 3, kPanelWidth, kPanelHeight,
                                panel_pixels);
}
