#include "diode_status.h"
#include "diode_feedback.h"
#include "diode_presence.h"
#include "battery_bridge.h"
#include "margin_status.h"
#include "board_settings.h"
#include "display_bus.h"
#include "foreground_bridge.h"
#include <esp_timer.h>

namespace {
using pw_stick::DiodeIcon;
using pw_stick::DiodeState;
StickConnection attempt = {};
DiodeIcon current = DiodeIcon::Hidden;
DiodeIcon candidate = DiodeIcon::Hidden;
unsigned confirmations = 0;
uint64_t next_check = UINT64_MAX;
uint64_t changed_at = 0;
uint64_t corrected_until = 0;
bool dismissed = false;

DiodeIcon classify(const pw_stick::DiodeProbe &probe) {
  if (!probe.receive_safe)
    return probe.state == DiodeState::Reversed ? DiodeIcon::Reversed : DiodeIcon::Blocked;
  switch (probe.state) {
  case DiodeState::Missing: return DiodeIcon::Missing;
  case DiodeState::Connected: return DiodeIcon::Detected;
  case DiodeState::Reversed: return DiodeIcon::Reversed;
  case DiodeState::Unknown: return DiodeIcon::Uncertain;
  }
  return DiodeIcon::Uncertain;
}
bool warning(DiodeIcon icon) {
  return icon == DiodeIcon::Missing || icon == DiodeIcon::Reversed ||
         icon == DiodeIcon::Blocked || icon == DiodeIcon::Uncertain;
}
}

extern "C" const StickConnection *StickLastConnection(void) { return &attempt; }
extern "C" void StickConnectionBegin(void) {
  attempt = {};
  attempt.started = 1;
  current = candidate = DiodeIcon::Hidden;
  confirmations = 0;
  next_check = UINT64_MAX;
  corrected_until = 0;
  dismissed = false;
  StickMarginInvalidate();
}
extern "C" int StickDiodePreflight(void) {
  const auto probe = pw_stick::diode_probe();
  attempt.checked = 1;
  attempt.preflight = unsigned(probe.state);
  attempt.receive_safe = probe.receive_safe;
  attempt.battery_mv = StickBatteryMillivolts();
  current = classify(probe);
  changed_at = uint64_t(esp_timer_get_time());
  StickMarginInvalidate();
  StickMarginDraw(true);
  return probe.receive_safe;
}
extern "C" void StickConnectionAcceptPacket(unsigned command, unsigned phase, unsigned bulk_phase) {
  ++attempt.packets;
  attempt.command = command;
  attempt.phase = phase;
  attempt.bulk_phase = bulk_phase;
  current = DiodeIcon::Signal;
}
extern "C" void StickConnectionFinish(unsigned result) {
  attempt.result = result;
  attempt.finished = 1;
  next_check = result && warning(current) ? uint64_t(esp_timer_get_time()) + 2000000 : UINT64_MAX;
  StickMarginInvalidate();
}
extern "C" void StickDiodeService(void) {
  if (StickForegroundIsIr()) return;
  if (StickSettingsIsOpen() || !StickDisplayIsPowered()) {
    next_check = UINT64_MAX;
    confirmations = 0;
    return;
  }
  if (!StickForegroundIsConnectionScreen()) {
    if (attempt.finished) dismissed = true;
    corrected_until = 0;
    next_check = UINT64_MAX;
    return;
  }
  if (dismissed || !attempt.finished || !attempt.result || !warning(current)) return;
  const uint64_t now = uint64_t(esp_timer_get_time());
  if (next_check == UINT64_MAX) next_check = now + 2000000;
  if (now < next_check) return;
  const auto probe = pw_stick::diode_probe();
  const auto observed = classify(probe);
  if (observed != candidate) { candidate = observed; confirmations = 0; }
  ++confirmations;
  if (!probe.receive_safe || (confirmations >= 2 && now - changed_at >= 1000000)) {
    if (current != observed) {
      current = observed;
      changed_at = uint64_t(esp_timer_get_time());
      if (current == DiodeIcon::Detected) corrected_until = changed_at + 2000000;
      StickMarginInvalidate();
    }
  }
  next_check = warning(current) ? uint64_t(esp_timer_get_time()) + 2000000 : UINT64_MAX;
}
extern "C" uint64_t StickDiodeNextDeadline(void) {
  if (StickSettingsIsOpen() || !StickDisplayIsPowered()) return UINT64_MAX;
  const uint64_t now = uint64_t(esp_timer_get_time());
  return corrected_until > now && corrected_until < next_check ? corrected_until : next_check;
}
extern "C" unsigned StickDiodeVisible(void) {
  if (!attempt.started || dismissed) return 0;
  if (!attempt.finished) return unsigned(current == DiodeIcon::Uncertain ? DiodeIcon::Hidden : current);
  if (!attempt.result || !StickForegroundIsConnectionScreen()) return 0;
  if (current == DiodeIcon::Detected && uint64_t(esp_timer_get_time()) >= corrected_until) return 0;
  return unsigned(current);
}
#ifdef PW_STICK_BENCH_CONTROL
extern "C" unsigned StickDiodeProbe(void) { return unsigned(pw_stick::diode_probe().state); }
extern "C" unsigned StickDiodeProbePin(unsigned pin) {
  if (pin < 6 || pin > 8) return unsigned(DiodeState::Unknown);
  return unsigned(pw_stick::diode_probe(pin).state);
}
#endif
