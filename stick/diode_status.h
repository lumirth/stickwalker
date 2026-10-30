#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/* A bounded weak-bias check, before the receiver can drive GPIO5 high. */
int StickDiodePreflight(void);
void StickConnectionBegin(void);
typedef struct {
  unsigned preflight;
  unsigned result;
  unsigned packets;
  unsigned command;
  unsigned phase;
  unsigned bulk_phase;
  unsigned battery_mv;
  int checked;
  int receive_safe;
  int started;
  int finished;
} StickConnection;
const StickConnection *StickLastConnection(void);
void StickConnectionAcceptPacket(unsigned command, unsigned phase, unsigned bulk_phase);
void StickConnectionFinish(unsigned result);
void StickDiodeService(void);
uint64_t StickDiodeNextDeadline(void);
unsigned StickDiodeVisible(void);
#ifdef PW_STICK_BENCH_CONTROL
unsigned StickDiodeProbe(void);
unsigned StickDiodeBiasProbe(void);
unsigned StickDiodeProbePin(unsigned pin);
#endif
#ifdef __cplusplus
}
#endif
