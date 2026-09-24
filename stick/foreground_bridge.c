#include "foreground_bridge.h"

#include "project.h"
#include "flags.h"
#include "application/pw_main.h"
#include "application/pw_home.h"
#include "application/pw_power.h"
#include "application/pw_nt7508.h"
#include "support/ir.h"

void RtcQuarterSecondInterrupt(void);
void RtcSecondInterrupt(void);
void BeepTick(void);

void StickForegroundQuarterSecond(void) { RtcQuarterSecondInterrupt(); }
void StickForegroundSecond(void) { RtcSecondInterrupt(); }
int StickForegroundIsIr(void) { return g_task == IrProtocolTick; }
int StickForegroundIsMain(void) { return g_task == MainTick; }
int StickForegroundIsBeep(void) { return g_task == BeepTick; }
unsigned StickForegroundUiFrame(void) { return g_state.uiFrame; }
void StickForegroundRun(void) {
  if (g_task) g_task();
#ifdef PW_STICK_BENCH_CONTROL
  /* The bench command represents one button request, not a latched input. */
  if (g_task == IrProtocolTick) g_state.events.byte &= EVENT_CLEAR(EVENT_IR_REQUEST);
#endif
}
void StickForegroundRequestIr(void) {
#ifdef PW_STICK_BENCH_CONTROL
  /* This is the same source entry used by the registered walker's Connect
   * menu. The event shortcut is only consumed by the unregistered home. */
  TryBeginIr();
#else
  g_state.events.byte |= EVENT_IR_REQUEST;
#endif
}
void StickForegroundWakeDisplay(void) { MotionSessionStart(); }
void StickForegroundDeviceMenuClosed(void) { StickForegroundWakeDisplay(); }
void StickForegroundCenterWake(void) {
  /* The original center switch also raises IRQ0. The GPIO adapter must
   * supply that latch when its virtual Center edge is first delivered. */
  g_state.events.byte |= EVENT_CENTER_PRESS;
  g_state.buttonWake[0] = 1;
}
#ifdef PW_STICK_BENCH_CONTROL
void StickForegroundBenchSleep(void) {
  DisplayEnterPowerSave();
  g_state.flags.byte = (g_state.flags.byte & SYSTEM_MODE_CLEAR) |
                       SYSTEM_MODE_MOTION;
  g_state.idleSeconds[IDLE_DISPLAY] = 0;
  g_state.buttonWake[0] = 0;
  g_state.centerHoldScans = 0;
}
#endif
unsigned StickForegroundIrResult(void) { return g_state.irResult; }
void StickForegroundIrDiagnostic(unsigned *phase, unsigned *received,
                                 unsigned *reference) {
  *phase = g_work.irc.work.handshakePhase;
  *received = g_state.irReceivedBytes;
  *reference = g_state.irTimerReference;
}
#ifdef PW_STICK_BENCH_CONTROL
void StickForegroundUiDiagnostic(unsigned *view, unsigned *updates,
                                 unsigned *frames, unsigned *seconds,
                                 unsigned *flags, unsigned *idle,
                                 unsigned *menu_selection,
                                 unsigned *pressed_buttons) {
  *view = g_state.view;
  *updates = g_state.viewUpdates;
  *frames = g_state.uiFrame;
  *seconds = g_state.save.rtcSeconds;
  *flags = g_state.flags.byte;
  *idle = g_state.idleSeconds[IDLE_DISPLAY];
  *menu_selection = g_state.menuSelection;
  *pressed_buttons = g_state.pressedButtons;
}
#endif
