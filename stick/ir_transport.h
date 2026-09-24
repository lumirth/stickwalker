#ifndef PW_STICK_IR_TRANSPORT_H
#define PW_STICK_IR_TRANSPORT_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hardware boundary for the original src/support/ir.c state machine.
 * The physical receiver delivers complete raw 115200-baud UART/SIR bursts
 * in arrival order, closed by a payload-blind optical/UART gap.
 * The native protocol remains responsible for XOR 0xAA, packet boundaries,
 * checksums, commands, and session state. The TX entry points accept logical
 * bytes and apply the transport XOR before emitting SIR.
 *
 * SendFrame returns only after the final wire stop bit. The adapter owns the
 * bounded burst queue. It must never accept self-echo
 * from a local transmission. A queue overflow or physical transport fault is
 * sticky until StickIrStart and is reported by StickIrFailed. */
void StickIrInitPins(void);
void StickIrConfigure(void);
void StickIrStart(void);
void StickIrPassiveDiagnostic(int enabled);
void StickIrStop(void);
u16 StickIrTicks(void);
void StickIrDelayTicks(u16 ticks);
void StickIrSendByte(u8 logical);
void StickIrSendFrame(const u8 *logical, u8 length);
/* Take at most one complete burst. wire has capacity bytes; length includes
 * every byte and lastObservationTick is the last optical observation on the
 * 32768-Hz, wrapping native Timer W scale. A burst larger than capacity sets
 * the sticky failure flag instead of silently truncating. */
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
