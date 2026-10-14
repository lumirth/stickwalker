#include "diode_status.h"
#include "diode_feedback.h"
#include "diode_presence.h"
#include "board_hal.h"
#include "board_settings.h"
#include "display_bus.h"
#include "display_panel.h"
#include "foreground_bridge.h"
#include "input_bridge.h"
#include <Arduino.h>
#include <M5Unified.h>
#include <esp_timer.h>

namespace {
using pw_stick::DiodeState;
pw_stick::DiodeFeedback feedback;
bool dirty = true;
bool blocked = false;
unsigned rechecks = 0;
uint64_t next_check = UINT64_MAX;
DiodeState drawn = DiodeState::Hidden;

void draw()
{
  if (!StickDisplayPanelIsReady() || StickSettingsIsOpen()) return;
  const DiodeState visible = StickDisplayIsPowered()
      ? feedback.visible(StickForegroundIsConnectionScreen()) : DiodeState::Hidden;
  if (!dirty && visible == drawn) return;
  auto *screen = StickBoardScreen();
  const unsigned x = pw_stick::diode_icon_x(StickInputOrientation());
  const unsigned y = pw_stick::diode_icon_y(StickInputOrientation());
  uint16_t pixels[16 * 16];
  const uint16_t background = StickDisplayBackground();
  const uint16_t foreground = StickDisplayForeground();
  for (unsigned row = 0; row < 16; ++row) {
    const uint16_t bits = visible == DiodeState::Hidden
        ? 0 : pw_stick::kDiodeIcons[unsigned(visible) - 1][row];
    for (unsigned col = 0; col < 16; ++col)
      pixels[row * 16 + col] = bits & (1u << col) ? foreground : background;
  }
  screen->pushImage(x, y, 16, 16, pixels);
  drawn = visible;
  dirty = false;
}
} // namespace

extern "C" void StickDiodeBegin(void)
{
  feedback.begin();
  blocked = false;
  rechecks = 0;
  next_check = UINT64_MAX;
  dirty = true;
}

extern "C" int StickDiodePreflight(void)
{
  const auto probe = pw_stick::diode_probe();
  feedback.observe(probe.state);
  blocked = !probe.receive_safe;
  dirty = true;
  draw();
  return probe.receive_safe;
}

extern "C" void StickDiodeReceivedPacket(void)
{
  feedback.observe(DiodeState::Connected);
}

extern "C" void StickDiodeEnd(unsigned)
{
  feedback.end();
  dirty = true;
  if (blocked) {
    rechecks = 3;
    next_check = uint64_t(esp_timer_get_time()) + 500000;
  }
}

extern "C" void StickDiodeService(void)
{
  if (StickForegroundIsIr()) return;
  if (StickSettingsIsOpen() || !StickForegroundIsConnectionScreen()) {
    feedback.clear();
    next_check = UINT64_MAX;
    rechecks = 0;
  }
  if (!StickDisplayIsPowered()) {
    next_check = UINT64_MAX;
    rechecks = 0;
    return;
  }
  const uint64_t now = uint64_t(esp_timer_get_time());
  if (now < next_check) return;
  --rechecks;
  const auto probe = pw_stick::diode_probe();
  if (probe.state != DiodeState::Unknown) {
    feedback.observe(probe.state);
    dirty = true;
  }
  if (probe.state == DiodeState::Connected) rechecks = 0;
  next_check = rechecks ? now + 500000 : UINT64_MAX;
}

extern "C" uint64_t StickDiodeNextDeadline(void) { return next_check; }
extern "C" void StickDiodeInvalidate(void) { dirty = true; }
extern "C" void StickDiodeDraw(void)
{
  if (!StickForegroundIsIr()) draw();
}

#ifdef PW_STICK_BENCH_CONTROL
extern "C" unsigned StickDiodeProbe(void)
{
  return unsigned(pw_stick::diode_probe().state);
}
extern "C" unsigned StickDiodeProbePin(unsigned pin)
{
  if (pin < 6 || pin > 8) return unsigned(DiodeState::Unknown);
  return unsigned(pw_stick::diode_probe(pin).state);
}
#endif
