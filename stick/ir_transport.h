#ifndef PW_STICK_IR_TRANSPORT_H
#define PW_STICK_IR_TRANSPORT_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Receive complete raw 115200-baud UART/SIR bursts in arrival order. Optical
 * gaps delimit bursts; src/support/ir.c decodes the XOR 0xAA bytes and checks
 * packets and session state. TX accepts logical bytes, applies XOR and emits
 * SIR. SendFrame waits for the final stop bit. Local transmission suppresses
 * self-echo. Queue overflow and transport faults set StickIrFailed until
 * StickIrStart clears it. */
void StickIrInitPins(void);
void StickIrConfigure(void);
void StickIrStart(void);
void StickIrPassiveDiagnostic(int enabled);
void StickIrStop(void);
void StickIrResumeReception(void);
u16 StickIrTicks(void);
void StickIrDelayTicks(u16 ticks);
void StickIrSendByte(u8 logical);
void StickIrSendFrame(const u8 *logical, u8 length);
/* Read one complete burst into wire, which has capacity bytes. length
 * receives the byte count; lastObservationTick receives the final optical
 * observation on the wrapping 32768 Hz Timer W clock. An oversized burst sets
 * the failure flag. */
int StickIrTakeBurst(u8 *wire, u8 capacity, u8 *length,
                     u16 *lastObservationTick);
int StickIrFailed(void);
#ifdef PW_STICK_BENCH_CONTROL
void StickIrBenchTrace(int enabled);
int StickIrBenchTraceEnabled(void);
void StickIrTraceChecksumFailure(const u8 *bytes, u8 length,
                                u16 received, u16 computed);
void StickIrTraceMalformedPage(const u8 *bytes, u8 length);
#endif

#ifdef __cplusplus
}
#endif

#endif
