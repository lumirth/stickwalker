#ifndef STICKWALKER_INFRARED_CAPTURE_H
#define STICKWALKER_INFRARED_CAPTURE_H

#include "infrared_timing.h"
#define INFRARED_CAPTURE_LISTEN_MODE 1

#define CAPTURE_FLOATING_MUX_OFFSET 0
#define CAPTURE_FLOATING_READS_OFFSET 4
#define CAPTURE_PULLED_READS_OFFSET 8
#define CAPTURE_PREVIOUS_RELEASE_OFFSET 12
#define CAPTURE_STREAM_OFFSET 16

#define STREAM_WRITE_COUNT_OFFSET 0
#define STREAM_READ_COUNT_OFFSET 4
#define STREAM_REQUESTED_MODE_OFFSET 8
#define STREAM_OBSERVATIONS_OFFSET 12

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

#include "infrared_decoder.h"

typedef struct {
    volatile uint32_t *write_count;
    volatile uint32_t *read_count;
    volatile uint32_t *requested_mode;
    InfraredObservation *observations;
} InfraredCaptureStream;

typedef struct {
    uint32_t floating_mux;
    uint32_t floating_reads;
    uint32_t pulled_reads;
    uint32_t previous_release;
    const InfraredCaptureStream *stream;
} InfraredCaptureSettings;

_Static_assert(sizeof(InfraredObservation) == 12, "Assembly observation layout");
_Static_assert(offsetof(InfraredObservation, duration) == 4, "Assembly duration field");
_Static_assert(offsetof(InfraredObservation, flags) == 6, "Assembly status field");
_Static_assert(offsetof(InfraredObservation, sample_gap_cycles) == 7, "Assembly sample interval field");
_Static_assert(offsetof(InfraredObservation, signal_duration) == 8, "Assembly light measurement");
_Static_assert(offsetof(InfraredObservation, floating_cycles) == 10, "Assembly floating measurement");
_Static_assert(sizeof(InfraredCaptureSettings) == 20, "Assembly capture settings");
_Static_assert(offsetof(InfraredCaptureSettings, floating_mux) == CAPTURE_FLOATING_MUX_OFFSET,
               "Assembly floating mux field");
_Static_assert(offsetof(InfraredCaptureSettings, floating_reads) == CAPTURE_FLOATING_READS_OFFSET,
               "Assembly floating read count");
_Static_assert(offsetof(InfraredCaptureSettings, pulled_reads) == CAPTURE_PULLED_READS_OFFSET,
               "Assembly pulled read count");
_Static_assert(offsetof(InfraredCaptureSettings, previous_release) == CAPTURE_PREVIOUS_RELEASE_OFFSET,
               "Assembly release field");
_Static_assert(offsetof(InfraredCaptureSettings, stream) == CAPTURE_STREAM_OFFSET,
               "Assembly stream field");
_Static_assert(offsetof(InfraredCaptureStream, write_count) == STREAM_WRITE_COUNT_OFFSET,
               "Assembly producer cursor");
_Static_assert(offsetof(InfraredCaptureStream, read_count) == STREAM_READ_COUNT_OFFSET,
               "Assembly consumer cursor");
_Static_assert(offsetof(InfraredCaptureStream, requested_mode) == STREAM_REQUESTED_MODE_OFFSET,
               "Assembly receive mode");
_Static_assert(offsetof(InfraredCaptureStream, observations) == STREAM_OBSERVATIONS_OFFSET,
               "Assembly observation queue");

void infrared_capture_batch(InfraredObservation records[INFRARED_CAPTURE_BATCH],
                            InfraredCaptureSettings *settings);
#endif

#endif
