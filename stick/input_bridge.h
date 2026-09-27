#ifndef PW_STICK_INPUT_BRIDGE_H
#define PW_STICK_INPUT_BRIDGE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void StickInputInit(void);
/* Poll physical levels frequently enough to queue taps and resolve chords. */
void StickInputPoll(unsigned long milliseconds);
/* Consume one native Pokewalker input scan, returning BUTTON_* bits. */
u8 StickInputLevels(void);
/* True while a physical press or a queued gesture needs prompt native scans. */
int StickInputWakeScanActive(void);
/* Dark game with no device overlay: only physical M begins a wake gesture. */
int StickInputMOnlyWake(void);
int StickMenuRequested(void);
/* Device overlay consumes physical M/R/L without sending game input. */
void StickInputMenuMode(int enabled);
u8 StickInputTakeMenuButtons(void);
u8 StickInputProfile(void);
/* Four independent layouts; profile retains the 0=two/1=three diagnostic. */
u8 StickInputLayout(void);
u8 StickInputOrientation(void);
u8 StickInputChordWindowIndex(void);
int StickInputConfigure(u8 layout, u8 orientation, u8 chord_window_index);
#ifdef PW_STICK_BENCH_CONTROL
void StickInputBenchInject(u8 button);
void StickInputBenchMainHold(unsigned milliseconds);
void StickInputBenchPowerEvent(void);
void StickInputBenchHold(u8 button, unsigned scans);
void StickInputDiagnostic(unsigned *main_edges, unsigned *side_edges,
                          unsigned *power_edges, unsigned *power_event_count,
                          unsigned *pmic_errors,
                          unsigned *last_raw, unsigned *last_logical,
                          unsigned *power_ready);
void StickInputPathDiagnostic(unsigned *stable, unsigned *gesture,
                             unsigned *desired, unsigned *delivered,
                             unsigned *queued, unsigned *wait_release,
                             unsigned *emitted, unsigned *consumed,
                             unsigned *overflow, unsigned *left_edges,
                             unsigned *right_edges, unsigned *center_edges);
#endif

#ifdef __cplusplus
}
#endif

#endif
