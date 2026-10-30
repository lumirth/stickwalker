#include "foreground_bridge.h"
#include "native_runtime.h"

#include "project.h"
#include "flags.h"
#include "application/pw_main.h"
#include "application/pw_home.h"
#include "application/pw_power.h"
#include "application/pw_nt7508.h"
#include "application/pw_buzzer.h"
#include "support/ir.h"

void BeepTick(void);
void MotionSessionEnd(void);

int StickForegroundIsIr(void) { return g_task == IrProtocolTick; }
int StickForegroundIsConnectionScreen(void) {
  return g_state.view == VIEW_IR_RESULT;
}
int StickForegroundIsMain(void) { return g_task == MainTick; }
int StickForegroundIsBeep(void) { return g_task == BeepTick; }
int StickForegroundIsInactive(void) {
  return (g_state.flags.byte & SYSTEM_MODE_MASK) == SYSTEM_MODE_INACTIVE;
}
void StickForegroundRun(void) {
  if (g_task) g_task();
#ifdef PW_STICK_BENCH_CONTROL
  /* Consume each bench button request once. */
  if (g_task == IrProtocolTick) g_state.events.byte &= EVENT_CLEAR(EVENT_IR_REQUEST);
#endif
}
void StickForegroundRequestIr(void) {
#ifdef PW_STICK_BENCH_CONTROL
  /* Enter the registered walker's Connect menu directly. Unregistered walkers
   * use the home-screen event. */
  TryBeginIr();
#else
  g_state.events.byte |= EVENT_IR_REQUEST;
#endif
}
void StickForegroundDeviceMenuClosed(void) {
  StickNativeMotionClock(1);
  MotionSessionStart();
}
void StickForegroundCenterWake(void) {
  StickNativeCenterInterrupt();
  StickNativeService();
}
void StickForegroundAcceptCenterInterrupt(void) {
  /* The original center switch also raises IRQ0. The GPIO adapter must
   * supply that latch when its virtual Center edge is first delivered. */
  g_state.events.byte |= EVENT_CENTER_PRESS;
  g_state.buttonWake[0] = 1;
}
void StickForegroundDiscardCenterInput(void) {
  StickNativeCancelCenterInterrupt();
  g_state.events.byte &= EVENT_CLEAR(EVENT_CENTER_PRESS);
}
int StickForegroundIrHasInput(void) { return g_state.irReceivedBytes != 0; }
#ifdef PW_STICK_BENCH_CONTROL
void StickForegroundBenchSleep(void) {
  DisplayEnterPowerSave();
  g_state.flags.byte = (g_state.flags.byte & SYSTEM_MODE_CLEAR) |
                       SYSTEM_MODE_MOTION;
  g_state.idleSeconds[IDLE_DISPLAY] = 0;
  g_state.buttonWake[0] = 0;
  g_state.centerHoldScans = 0;
}
void StickForegroundBenchInactive(void) {
  DisplayEnterPowerSave();
  MotionSessionEnd();
  g_state.idleSeconds[IDLE_DISPLAY] = 0;
  g_state.idleSeconds[IDLE_MOTION] = 0;
  g_state.buttonWake[0] = 0;
  g_state.centerHoldScans = 0;
}
int StickForegroundBenchMoveScore(void) {
  if (!StickForegroundIsMain()) return 0;
  BeepLoadScore(SCORE_MOVE);
  if (!BeepHasScore()) return 0;
  InstallTask(BeepTick);
  BeepEnableTimer();
  return 1;
}
#endif
unsigned StickForegroundIrResult(void) { return g_state.irResult; }
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
