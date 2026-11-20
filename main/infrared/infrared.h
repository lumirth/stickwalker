#ifndef STICKWALKER_INFRARED_H
#define STICKWALKER_INFRARED_H

#include "pokewalker_host.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void pw_host_cycle_clock(bool enabled);

#define INFRARED_MAX_FRAME 136u

enum {
    INFRARED_ERROR_FRAMING = 1u,
    INFRARED_ERROR_OVERRUN = 2u,
    INFRARED_ERROR_ACQUISITION_GAP = 4u
};

typedef enum {
    INFRARED_EVENT_BYTE,
    INFRARED_EVENT_QUIET
} InfraredEventKind;

typedef struct {
    uint64_t time_us;
    uint8_t value;
    uint8_t errors;
    InfraredEventKind kind;
} InfraredEvent;

/* The board checks the diode input, supplies stable emitter power, and
 * disables its speaker before opening. */
bool infrared_open(void);
bool infrared_close(void);
bool infrared_receive(InfraredEvent *event);
bool infrared_transmit(const uint8_t *bytes, size_t length, uint64_t *ended_us);

void infrared_stop(void);
void infrared_yield(void);
bool infrared_can_yield(void);

#ifdef __cplusplus
}
#endif
#endif
