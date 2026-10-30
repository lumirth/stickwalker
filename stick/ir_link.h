#ifndef PW_STICK_IR_LINK_H
#define PW_STICK_IR_LINK_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void StickIrInitPins(void);
void StickIrConfigure(void);
void StickIrStart(void);
void StickIrPassiveDiagnostic(int enabled);
void StickIrStop(void);

// Only the native session may grant a service turn. It calls this after
// validating a request that permits a reply, storage work, or disconnect.
// Return after both cores have their interrupts restored and GPIO5 is idle.
int StickIrBeginService(void);
int StickIrStorageAllowed(void);
int StickIrReceivingFrame(void);
void StickIrResumeReception(void);

u16 StickIrTicks(void);
void StickIrDelayTicks(u16 ticks);
void StickIrSendByte(u8 logical);
void StickIrSendFrame(const u8 *logical, u8 length);
/* Pure candidate admission: 0 rejects, 1 admits an unchecksummed probe,
 * and 2 admits a complete checked packet. It must not change session state. */
typedef unsigned (*StickIrPacketValidator)(const u8 *wire, unsigned length);
int StickIrTakePacket(u8 *wire, u8 capacity, u8 *length,
                      u16 *lastObservationTick, StickIrPacketValidator validate);
int StickIrFailed(void);
void StickIrAcceptPacket(unsigned command, unsigned phase, unsigned bulk_phase);
void StickIrRejectPacket(unsigned reason);
void StickIrSessionResult(unsigned result);
void StickIrServiceDiagnostics(void);
#ifdef PW_STICK_BENCH_CONTROL
void StickIrBenchTrace(int enabled);
int StickIrBenchTraceEnabled(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
