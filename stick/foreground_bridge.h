#ifndef PW_STICK_FOREGROUND_BRIDGE_H
#define PW_STICK_FOREGROUND_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

void StickForegroundQuarterSecond(void);
void StickForegroundSecond(void);
int StickForegroundIsIr(void);
int StickForegroundIsMain(void);
int StickForegroundIsBeep(void);
unsigned StickForegroundUiFrame(void);
void StickForegroundRun(void);
void StickForegroundRequestIr(void);
void StickForegroundDeviceMenuClosed(void);
void StickForegroundWakeDisplay(void);
void StickForegroundCenterWake(void);
#ifdef PW_STICK_BENCH_CONTROL
void StickForegroundBenchSleep(void);
#endif
unsigned StickForegroundIrResult(void);
void StickForegroundIrDiagnostic(unsigned *phase, unsigned *received,
                                 unsigned *reference);
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
