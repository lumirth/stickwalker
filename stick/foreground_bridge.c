#include "foreground_bridge.h"

#include "project.h"
#include "flags.h"
#include "application/pw_main.h"
#include "support/ir.h"

void RtcQuarterSecondInterrupt(void);
void RtcSecondInterrupt(void);
void BeepTick(void);

void StickForegroundQuarterSecond(void) { RtcQuarterSecondInterrupt(); }
void StickForegroundSecond(void) { RtcSecondInterrupt(); }
int StickForegroundIsIr(void) { return g_task == IrProtocolTick; }
int StickForegroundIsMain(void) { return g_task == MainTick; }
int StickForegroundIsBeep(void) { return g_task == BeepTick; }
void StickForegroundRun(void) {
  if (g_task) g_task();
#ifdef PW_STICK_BENCH_CONTROL
  /* The bench command represents one button request, not a latched input. */
  if (g_task == IrProtocolTick) g_state.events.byte &= EVENT_CLEAR(EVENT_IR_REQUEST);
#endif
}
void StickForegroundRequestIr(void) { g_state.events.byte |= EVENT_IR_REQUEST; }
unsigned StickForegroundIrResult(void) { return g_state.irResult; }
void StickForegroundIrDiagnostic(unsigned *phase, unsigned *received,
                                 unsigned *reference) {
  *phase = g_work.irc.work.handshakePhase;
  *received = g_state.irReceivedBytes;
  *reference = g_state.irTimerReference;
}
