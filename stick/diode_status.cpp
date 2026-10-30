#include "diode_status.h"
#include "diode_feedback.h"
#include "margin_status.h"
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
bool blocked = false;
unsigned rechecks = 0;
uint64_t next_check = UINT64_MAX;
uint64_t retain_until = 0;
} // namespace

extern "C" void StickDiodeBegin(void)
{
  feedback.begin();
  blocked = false;
  rechecks = 0;
  next_check = UINT64_MAX;
  StickMarginInvalidate();
}

extern "C" int StickDiodePreflight(void)
{
  const auto probe = pw_stick::diode_probe();
  feedback.observe(probe.receive_safe ? probe.state :
      probe.state == DiodeState::Reversed ? DiodeState::Reversed : DiodeState::Blocked);
  blocked = !probe.receive_safe;
  StickMarginInvalidate();
  StickMarginDraw(true);
  return probe.receive_safe;
}

extern "C" void StickDiodeReceivedPacket(void)
{
  feedback.observe(DiodeState::Receiving);
}

extern "C" void StickDiodeEnd(unsigned)
{
  feedback.end();
  retain_until = uint64_t(esp_timer_get_time()) + 3000000;
  StickMarginInvalidate();
  if (blocked) {
    rechecks = 3;
    next_check = uint64_t(esp_timer_get_time()) + 500000;
  }
}

extern "C" void StickDiodeService(void)
{
  if (StickForegroundIsIr()) return;
  if (StickSettingsIsOpen() || (!StickForegroundIsConnectionScreen() &&
      uint64_t(esp_timer_get_time()) >= retain_until)) {
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
  if (!rechecks) return;
  --rechecks;
  const auto probe = pw_stick::diode_probe();
  if (probe.state != DiodeState::Unknown) {
    feedback.observe(probe.state);
    StickMarginInvalidate();
  }
  if (probe.state == DiodeState::Connected) rechecks = 0;
  next_check = rechecks ? now + 500000 : UINT64_MAX;
}

extern "C" uint64_t StickDiodeNextDeadline(void) { return next_check; }
extern "C" unsigned StickDiodeVisible(void) {
  return unsigned(feedback.visible(StickForegroundIsConnectionScreen() ||
      uint64_t(esp_timer_get_time()) < retain_until));
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
