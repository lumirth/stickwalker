#include "display_panel.h"

#include "display_bus.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>

namespace {

constexpr unsigned kNativeWidth = 96;
constexpr unsigned kNativeHeight = 64;
constexpr unsigned kScale = 2;
constexpr unsigned kPanelWidth = kNativeWidth * kScale;
constexpr unsigned kPanelHeight = kNativeHeight * kScale;
constexpr uint16_t kPalette[4] = {0xffff, 0xbdf7, 0x738e, 0x0000};

uint8_t native_pixels[kNativeWidth * kNativeHeight];
uint16_t *panel_pixels = nullptr;

}  // namespace

extern "C" int StickDisplayPanelInit(void) {
  M5.Display.setRotation(3);
  M5.Display.setBrightness(48);
  M5.Display.fillScreen(kPalette[0]);
  StickDisplayBusInit();
  if (!panel_pixels) {
    panel_pixels = static_cast<uint16_t *>(heap_caps_malloc(
        sizeof(uint16_t) * kPanelWidth * kPanelHeight,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  return panel_pixels != nullptr;
}

extern "C" void StickDisplayPresent(void) {
  if (!panel_pixels) return;
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
  M5.Display.pushImage(24, 3, kPanelWidth, kPanelHeight, panel_pixels);
}
