#ifndef STICKWALKER_INFRARED_DECODER_H
#define STICKWALKER_INFRARED_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "infrared_timing.h"

// The decoder runs from flash through the instruction cache: CPU0 fetches from
// IRAM delay the pin writes of the sampler on CPU1.
#define INFRARED_FAST_CODE

enum {
    INFRARED_OBSERVATION_RECONFIGURE = 0x40u,
    INFRARED_OBSERVATION_REARM = 0x80u,
    INFRARED_LOOKAHEAD_OBSERVATIONS = 32
};

typedef struct {
    uint32_t end_cycle;
    uint16_t duration;
    uint8_t flags;
    uint8_t sample_gap_cycles;
    // Light comparison removes measured floating-phase timing variation.
    // duration remains the actual elapsed interval used to locate pulses.
    uint16_t signal_duration;
    uint16_t floating_cycles;
} InfraredObservation;

typedef struct {
    uint32_t completed_cycle;
    uint8_t value;
    uint8_t errors;
} InfraredDecodedByte;

typedef struct {
    InfraredObservation observation;
    int32_t preceding_lower;
    // How far the observation falls below the background.
    int32_t depth;
    uint8_t quiet_count;
} InfraredDelayedObservation;

typedef struct {
    uint32_t earliest;
    uint32_t latest;
    uint32_t saturated_crossing;
    bool saturated;
    bool active;
    bool separated;
} InfraredPulse;

typedef struct {
    uint32_t previous_end;
    uint32_t processed_cycle;
    uint32_t background_cycle;
    int32_t background_lower;
    int32_t background_upper;
    uint32_t background_q8;
    uint32_t jitter_allowance_q8;
    uint32_t floating_minimum;
    uint32_t floating_compensation_q8;
    unsigned sample_count;
    bool background_ready;
    bool receiving_byte;
    // After a saturating flash, light that only fades is the diode recovering.
    bool recovering;
    int32_t previous_depth;
    bool quiet_ready;
    uint32_t quiet_bound;
    InfraredPulse pulse;
    uint32_t start_earliest;
    uint32_t start_latest;
    uint32_t start_reference;
    uint16_t zero_bits;
    unsigned delayed_head;
    unsigned ready_count;
    unsigned prior_count;
    unsigned current_count;
    uint32_t block_start;
    uint32_t block_future_key;
    uint32_t block_past_key;
    uint32_t prior_past_key;
    uint16_t block_upper_minimum;
    int16_t block_lower_maximum;
    uint8_t block_gap;
    bool block_uniform;
    uint16_t prior_upper_minimum;
    int16_t prior_lower_maximum;
    InfraredDelayedObservation delayed[INFRARED_LOOKAHEAD_OBSERVATIONS];
} InfraredDecoder;

void infrared_decoder_init(InfraredDecoder *decoder);
void infrared_decoder_measure_noise(InfraredDecoder *decoder,
                                    const InfraredObservation *observations, unsigned count);
void infrared_decoder_rearm(InfraredDecoder *decoder);
bool infrared_decoder_consume(InfraredDecoder *decoder, const InfraredObservation *observations,
                              unsigned count, unsigned *consumed, InfraredDecodedByte *byte);
size_t infrared_encode_frame(const uint8_t *bytes, size_t length, uint32_t *symbols,
                             size_t capacity, uint32_t *last_pulse_tick, uint32_t *total_ticks);

#endif
