#ifndef PW_STICK_IR_TRANSPORT_H
#define PW_STICK_IR_TRANSPORT_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The physical link continuously acquires raw gates while listening. Packet
 * rejection never parks acquisition. Native session code alone may grant a
 * service window after preserving a request whose sender awaits our reply.
 * BeginService parks capture and lets both cores service interrupts before
 * returning. EndService waits for fresh capture; SendFrame calls it itself.
 * A keepalive or ignored command does not grant a service window. During
 * exclusive capture the dedicated RTOS tick clock is paused; native wall
 * time and the progress-supervised hardware watchdogs continue running. */
void StickIrInitPins(void);
void StickIrConfigure(void);
void StickIrStart(void);
void StickIrPassiveDiagnostic(int enabled);
void StickIrStop(void);
int StickIrBeginService(void);
void StickIrEndService(void);
u16 StickIrTicks(void);
void StickIrDelayTicks(u16 ticks);
void StickIrSendByte(u8 logical);
void StickIrSendFrame(const u8 *logical, u8 length);

/* Read one complete UART frame in arrival order. Wire bytes still carry XOR
 * AA. lastObservationTick is the observed final UART stop completion, on the
 * native wrapping 32768 Hz clock. It is independent of dequeue time. */
int StickIrTakeBurst(u8 *wire, u8 capacity, u8 *length,
                     u16 *lastObservationTick);
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
