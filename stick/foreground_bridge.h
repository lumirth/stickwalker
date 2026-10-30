#ifndef PW_STICK_FOREGROUND_BRIDGE_H
#define PW_STICK_FOREGROUND_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

int StickForegroundIsIr(void);
int StickForegroundIsConnectionScreen(void);
int StickForegroundIsMain(void);
int StickForegroundIsBeep(void);
int StickForegroundIsInactive(void);
void StickForegroundRun(void);
void StickForegroundRequestIr(void);
void StickForegroundDeviceMenuClosed(void);
void StickForegroundCenterWake(void);
void StickForegroundAcceptCenterInterrupt(void);
void StickForegroundDiscardCenterInput(void);
#ifdef PW_STICK_BENCH_CONTROL
void StickForegroundBenchSleep(void);
void StickForegroundBenchInactive(void);
int StickForegroundBenchMoveScore(void);
#endif
unsigned StickForegroundIrResult(void);
#ifdef PW_STICK_BENCH_CONTROL
void StickForegroundUiDiagnostic(unsigned *view, unsigned *updates,
                                 unsigned *frames, unsigned *seconds,
                                 unsigned *flags, unsigned *idle,
                                 unsigned *menu_selection,
                                 unsigned *pressed_buttons);
#endif

#ifdef __cplusplus
}
#endif

#endif
