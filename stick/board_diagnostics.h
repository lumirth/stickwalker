#pragma once

#include <stdint.h>

#ifdef PW_STICK_BENCH_CONTROL
void StickDiagnosticsInit(void);
int StickDiagnosticsService(void);
void StickDiagnosticsForeground(unsigned task, uint64_t elapsed);
void StickDiagnosticsInput(uint64_t elapsed);
void StickDiagnosticsSound(uint64_t elapsed);
void StickDiagnosticsPresent(uint64_t elapsed);
void StickDiagnosticsBeforeSleep(int usb_present);
#else
inline void StickDiagnosticsInit(void) {}
inline int StickDiagnosticsService(void) { return 0; }
inline void StickDiagnosticsForeground(unsigned, uint64_t) {}
inline void StickDiagnosticsInput(uint64_t) {}
inline void StickDiagnosticsSound(uint64_t) {}
inline void StickDiagnosticsPresent(uint64_t) {}
inline void StickDiagnosticsBeforeSleep(int) {}
#endif
