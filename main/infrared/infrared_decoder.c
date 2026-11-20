#include "infrared_decoder.h"
#include "infrared.h"
#include <string.h>
#define CAPTURE_TIMESTAMP_TRAIL_CYCLES 4
// One duration cycle per 17 us follows lighting drift without following a
// minimum-width SIR flash. Circuit sweeps include 120 Hz ripple and shadows.
#define BACKGROUND_SCALE 4096
#define NOMINAL_INPUT_VARIATION (2u * INFRARED_INPUT_JITTER_CYCLES)
#define MAXIMUM_QUIET_VARIATION 32u
#define LOOKAHEAD_CYCLES INFRARED_BIT_CYCLES
#define MINIMUM_BIT_CYCLES 2062u
#define MAXIMUM_BIT_CYCLES 2105u
#define PULSE_SAMPLES 8u
#define PULSE_GAP_CYCLES 14u

static inline bool later(uint32_t first, uint32_t second)
{
    return (int32_t)(first - second) > 0;
}

void INFRARED_FAST_CODE infrared_decoder_init(InfraredDecoder *decoder)
{
    memset(decoder, 0, sizeof(*decoder));
    // Allow for the synchronizer uncertainty at both ends of a comparison.
    decoder->jitter_allowance_q8 = NOMINAL_INPUT_VARIATION * 256u;
    decoder->floating_compensation_q8 = 256;
}

void infrared_decoder_measure_noise(InfraredDecoder *decoder,
                                    const InfraredObservation *observations, unsigned count)
{
    // A peer may already be transmitting. Measure short quiet windows and
    // trim the upper quarter: partial pulses can fit below the span limit too.
    // A weak discovery probe shortens only a few samples, by less than the
    // span limit, so a window drops samples that stand apart from the rest.
    unsigned spans[MAXIMUM_QUIET_VARIATION + 1] = {0};
    unsigned windows = 0;
    int64_t variance_by_span[MAXIMUM_QUIET_VARIATION + 1] = {0};
    int64_t covariance_by_span[MAXIMUM_QUIET_VARIATION + 1] = {0};
    for (unsigned start = 0; start + 64 <= count; start += 64) {
        uint16_t lowest[PULSE_SAMPLES + 1];
        unsigned kept = 0;
        uint32_t highest_lower = 0;
        bool valid = true;
        for (unsigned i = start; i < start + 64; ++i) {
            const InfraredObservation *sample = &observations[i];
            if (sample->flags || !sample->sample_gap_cycles ||
                sample->signal_duration <= sample->sample_gap_cycles ||
                sample->duration > INFRARED_MAX_OBSERVATION_CYCLES) {
                valid = false;
                break;
            }
            uint32_t lower = sample->signal_duration - sample->sample_gap_cycles;
            unsigned j = kept < PULSE_SAMPLES + 1 ? kept++ : PULSE_SAMPLES + 1;
            for (; j && lowest[j - 1] > sample->signal_duration; --j) {
                if (j <= PULSE_SAMPLES) lowest[j] = lowest[j - 1];
            }
            if (j <= PULSE_SAMPLES) lowest[j] = sample->signal_duration;
            if (lower > highest_lower) highest_lower = lower;
        }
        // A peer flash shortens a few samples far below the rest. Smooth noise,
        // ripple and bus stalls keep the window's true minimum.
        uint32_t lowest_upper = !valid ? 0
            : lowest[PULSE_SAMPLES] - lowest[0] >= PULSE_GAP_CYCLES ? lowest[PULSE_SAMPLES] : lowest[0];
        unsigned span = highest_lower > lowest_upper ? highest_lower - lowest_upper : 0;
        bool untrimmed_quiet = valid && highest_lower <= lowest[0] + MAXIMUM_QUIET_VARIATION;
        if (valid && span <= MAXIMUM_QUIET_VARIATION) {
            spans[span]++;
            windows++;
        }
        if (untrimmed_quiet) {
            int64_t n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
            unsigned least = UINT16_MAX;
            for (unsigned i = start; i < start + 64; ++i) {
                unsigned floating = observations[i].floating_cycles;
                if (floating && floating < least) least = floating;
            }
            for (unsigned i = start; i < start + 64; ++i) {
                const InfraredObservation *sample = &observations[i];
                if (!sample->floating_cycles) continue;
                // A delayed write can leave the pin already below threshold at the
                // first pulled read. Its signal is the measurement floor, not a slope.
                if (sample->floating_cycles > least &&
                    sample->sample_gap_cycles > sample->floating_cycles - least) continue;
                int64_t x = sample->floating_cycles, y = sample->signal_duration;
                n++; sx += x; sy += y; sxx += x * x; sxy += x * y;
            }
            variance_by_span[span] += n * sxx - sx * sx;
            covariance_by_span[span] += sx * sy - n * sxy;
        }
    }
    // Within-window covariance excludes slow lighting drift. Trim pulse-bearing
    // windows using the same quiet percentile before fitting mux-delay effects.
    int64_t variance = 0, covariance = 0;
    unsigned cumulative = 0;
    for (unsigned span = 0; windows && span <= MAXIMUM_QUIET_VARIATION; span++) {
        cumulative += spans[span];
        variance += variance_by_span[span];
        covariance += covariance_by_span[span];
        if (cumulative * 4 < windows * 3) continue;
        unsigned variation = span + NOMINAL_INPUT_VARIATION;
        if (variation > MAXIMUM_QUIET_VARIATION) variation = MAXIMUM_QUIET_VARIATION;
        decoder->jitter_allowance_q8 = variation * 256u;
        if (variance > 0) {
            uint64_t correction = covariance > 0 ? (uint64_t)covariance * 256 / variance : 0;
            decoder->floating_compensation_q8 = correction < 256 ? correction : 256;
        }
        break;
    }
}

// A delayed mux write integrates extra ambient current. Keep the raw lower
// bound and compensate the upper bound using the startup quiet-line slope.
static inline unsigned signal_upper(const InfraredDecoder *decoder,
                                    const InfraredObservation *observation)
{
    unsigned extra = observation->floating_cycles > decoder->floating_minimum
        ? observation->floating_cycles - decoder->floating_minimum : 0;
    return observation->signal_duration + ((extra * decoder->floating_compensation_q8 + 255) >> 8);
}

static void INFRARED_FAST_CODE reset_decoder(InfraredDecoder *decoder)
{
    uint32_t variation = decoder->jitter_allowance_q8;
    uint32_t floating = decoder->floating_minimum;
    uint32_t correction = decoder->floating_compensation_q8;
    infrared_decoder_init(decoder);
    decoder->jitter_allowance_q8 = variation;
    decoder->floating_minimum = floating;
    decoder->floating_compensation_q8 = correction;
}

void INFRARED_FAST_CODE infrared_decoder_rearm(InfraredDecoder *decoder)
{
    decoder->previous_end = 0;
    decoder->processed_cycle = 0;
    decoder->receiving_byte = false;
    decoder->quiet_ready = false;
    decoder->pulse.active = false;
    decoder->delayed_head = 0;
    decoder->ready_count = 0;
    decoder->prior_count = 0;
    decoder->current_count = 0;
    if (!decoder->background_ready) {
        decoder->sample_count = 0;
    }
}

static bool INFRARED_FAST_CODE __attribute__((noinline)) reject(InfraredDecoder *decoder, uint32_t cycle,
                                     uint8_t error, InfraredDecodedByte *byte)
{
    reset_decoder(decoder);
    byte->completed_cycle = cycle;
    byte->value = 0;
    byte->errors = error;
    return true;
}

/* Each illuminated observation bounds when its pulse could have begun.
 * Intersecting those intervals groups consecutive observations of one pulse.
 * The first pulse is the start bit; later pulses must fit a data-bit position
 * for a start time still possible at +/-1% serial-clock error. Optical edge
 * jitter expands each pulse's ideal UART onset separately from its width. */
static bool INFRARED_FAST_CODE finish_pulse(InfraredDecoder *decoder)
{
    const InfraredPulse *pulse = &decoder->pulse;
    uint32_t pulse_earliest = pulse->earliest - INFRARED_PULSE_JITTER_CYCLES;
    uint32_t pulse_latest = pulse->latest + INFRARED_PULSE_JITTER_CYCLES;
    if (!decoder->receiving_byte) {
        decoder->start_earliest = pulse_earliest;
        decoder->start_latest = pulse_latest;
        decoder->start_reference = pulse->earliest + (pulse->latest - pulse->earliest) / 2u;
        decoder->zero_bits = 1u;
        decoder->receiving_byte = true;
        return true;
    }
    uint32_t middle = pulse->earliest + (pulse->latest - pulse->earliest) / 2u;
    uint32_t elapsed = middle - decoder->start_reference;
    unsigned bit = (elapsed + INFRARED_BIT_CYCLES / 2) / INFRARED_BIT_CYCLES;
    if (bit >= 9) {
        return false;
    }
    // One flash can produce separated illuminated observations. Repeated
    // observations in the same bit are compatible only if their onset bounds
    // still intersect the byte's start interval below; the bit stays zero.
    uint32_t earliest = pulse_earliest - bit * MAXIMUM_BIT_CYCLES;
    uint32_t latest = pulse_latest - bit * MINIMUM_BIT_CYCLES;
    if (later(earliest, decoder->start_latest) || later(decoder->start_earliest, latest)) {
        return false;
    }
    if (later(earliest, decoder->start_earliest)) {
        decoder->start_earliest = earliest;
    }
    if (later(decoder->start_latest, latest)) {
        decoder->start_latest = latest;
    }
    decoder->zero_bits |= 1u << bit;
    return true;
}

static bool INFRARED_FAST_CODE __attribute__((noinline)) decode_observation(InfraredDecoder *decoder,
                                                   const InfraredObservation *observation,
                                                   bool illuminated, InfraredDecodedByte *byte)
{
    uint32_t release = observation->end_cycle - observation->duration;
    uint32_t crossing = observation->end_cycle - CAPTURE_TIMESTAMP_TRAIL_CYCLES;
    if (!illuminated && observation->duration >= INFRARED_MIN_PULSE_CYCLES) {
        decoder->quiet_bound = crossing - INFRARED_MIN_PULSE_CYCLES -
                               INFRARED_INPUT_JITTER_CYCLES;
        uint32_t from_release = release - INFRARED_RELEASE_LEAD_CYCLES;
        if (later(from_release, decoder->quiet_bound)) {
            decoder->quiet_bound = from_release;
        }
        decoder->quiet_ready = true;
    }
    uint32_t earliest = release - INFRARED_RELEASE_LEAD_CYCLES - INFRARED_MAX_PULSE_CYCLES;
    if (decoder->quiet_ready && !later(decoder->quiet_bound, earliest)) {
        /* Once the width bound is newer, the quiet witness is redundant.
         * Retire it during short quiet runs as well, before CCOUNT wraps. */
        decoder->quiet_ready = false;
    }
    if (!illuminated && !decoder->receiving_byte && !decoder->pulse.active) {
        return false;
    }
    uint32_t latest = crossing + INFRARED_INPUT_JITTER_CYCLES;
    if (illuminated && decoder->quiet_ready) {
        /* The final minimum-width portion of a background observation
         * cannot conceal the start of a supported flash that illuminates
         * the next measurement. Preserve that bound at byte boundaries. */
        if (later(decoder->quiet_bound, earliest)) {
            earliest = decoder->quiet_bound;
        }
        decoder->quiet_ready = false;
    }

    /* Retire the preceding byte before finishing a pulse belonging to the
     * next character. A long observation can straddle this boundary. */
    bool completed = false;
    uint32_t stop_end = decoder->start_latest + 9u * MAXIMUM_BIT_CYCLES +
                        INFRARED_PULSE_JITTER_CYCLES + INFRARED_MAX_PULSE_CYCLES +
                        INFRARED_RELEASE_LEAD_CYCLES;
    if (decoder->receiving_byte && !later(stop_end, release) &&
        (!decoder->pulse.active || later(decoder->pulse.earliest,
            decoder->start_latest + 9u * MAXIMUM_BIT_CYCLES +
            INFRARED_PULSE_JITTER_CYCLES))) {
        byte->completed_cycle = stop_end;
        byte->value = (uint8_t)(~(decoder->zero_bits >> 1) ^ 0xaau);
        byte->errors = 0;
        decoder->receiving_byte = false;
        completed = true;
    }

    if (decoder->pulse.active &&
        (later(earliest, decoder->pulse.latest) || (illuminated && decoder->pulse.separated))) {
        if (!finish_pulse(decoder)) {
            return reject(decoder, crossing, INFRARED_ERROR_FRAMING, byte);
        }
        decoder->pulse.active = false;
    }
    if (illuminated) {
        if (!decoder->pulse.active) {
            decoder->pulse.earliest = earliest;
            decoder->pulse.latest = latest;
            decoder->pulse.active = true;
            decoder->pulse.separated = false;
        } else {
            if (later(earliest, decoder->pulse.earliest)) {
                decoder->pulse.earliest = earliest;
            }
            if (later(decoder->pulse.latest, latest)) {
                decoder->pulse.latest = latest;
            }
        }
    } else if (decoder->pulse.active &&
               observation->duration >= INFRARED_MIN_PULSE_CYCLES) {
        /* A full background measurement separates two flashes even when
         * their conservative onset intervals overlap. Short noisy samples
         * cannot split a slow edge this way. */
        decoder->pulse.separated = true;
    }
    return completed;
}

static void INFRARED_FAST_CODE __attribute__((noinline)) acquire_background(InfraredDecoder *decoder,
                                                  const InfraredObservation *observation)
{
    uint32_t release = observation->end_cycle - observation->duration;
    int32_t upper = (int32_t)(signal_upper(decoder, observation) - CAPTURE_TIMESTAMP_TRAIL_CYCLES) * BACKGROUND_SCALE;
    int32_t lower = ((int32_t)observation->signal_duration - CAPTURE_TIMESTAMP_TRAIL_CYCLES - observation->sample_gap_cycles) * BACKGROUND_SCALE;
    uint32_t elapsed = release - decoder->background_cycle;
    if (!decoder->sample_count || elapsed > INFRARED_MAX_OBSERVATION_CYCLES * BACKGROUND_SCALE) {
        decoder->background_lower = lower;
        decoder->background_upper = upper;
    } else {
        decoder->background_lower -= elapsed;
        if (lower > decoder->background_lower) {
            decoder->background_lower = lower;
        }
        decoder->background_upper += elapsed;
        if (upper < decoder->background_upper) {
            decoder->background_upper = upper;
        }
    }
    decoder->background_cycle = release;
    if (decoder->background_lower - decoder->background_upper >
        (int32_t)(decoder->jitter_allowance_q8 * (BACKGROUND_SCALE / 256))) {
        decoder->sample_count = 0;
        return;
    }
    decoder->background_ready = ++decoder->sample_count >= 32u;
    decoder->background_q8 = upper / (BACKGROUND_SCALE / 256);
}

/* Durations use 1/BACKGROUND_SCALE-cycle units. Background may change by one
 * duration cycle per BACKGROUND_SCALE elapsed CPU cycles; propagation subtracts
 * elapsed cycles directly. A pulse shortens a measurement below that bound.
 *
 * Each block spans one serial bit. Incoming observations form current; the
 * next block supplies future context for prior; resolved prior becomes ready.
 * past_key = lower + release and future_key = lower - release summarize the
 * two propagation directions. Forward/backward passes visit only prior, and
 * a quiet run can retire together because it cannot contain a pulse. */
static void INFRARED_FAST_CODE __attribute__((noinline))
resolve_prior_block(InfraredDecoder *decoder, uint32_t future_key, int32_t newest_lower)
{
    int32_t maximum = decoder->prior_lower_maximum;
    if (decoder->block_lower_maximum > maximum) {
        maximum = decoder->block_lower_maximum;
    }
    if (newest_lower > maximum) {
        maximum = newest_lower;
    }
    const int32_t input_variation = (int32_t)(decoder->jitter_allowance_q8 * (BACKGROUND_SCALE / 256));
    bool all_quiet = decoder->prior_count && !decoder->ready_count &&
        maximum <= (int32_t)decoder->prior_upper_minimum + (int32_t)(decoder->jitter_allowance_q8 >> 8) &&
        decoder->background_lower <= ((int32_t)decoder->prior_upper_minimum - CAPTURE_TIMESTAMP_TRAIL_CYCLES) * BACKGROUND_SCALE +
                                     input_variation;
    if (all_quiet) {
        unsigned last = (decoder->delayed_head + decoder->prior_count - 1u) %
                        INFRARED_LOOKAHEAD_OBSERVATIONS;
        InfraredDelayedObservation *prior = &decoder->delayed[last];
        uint32_t release = prior->observation.end_cycle - prior->observation.duration;
        uint32_t elapsed = release - decoder->background_cycle;
        int32_t lower = (int32_t)(decoder->prior_past_key - release);
        if (elapsed <= INFRARED_MAX_OBSERVATION_CYCLES * BACKGROUND_SCALE) {
            int32_t preceding = decoder->background_lower - (int32_t)elapsed;
            if (preceding > lower) {
                lower = preceding;
            }
        }
        int32_t ceiling = decoder->background_lower + (int32_t)elapsed;
        decoder->background_lower = lower < ceiling ? lower : ceiling;
        decoder->background_cycle = release;
        prior->quiet_count = 1;
        goto prepared;
    }
    // Apply the same drift bound in both directions. One long measurement
    // must not raise the reference instantly and turn ordinary quiet samples
    // into light; the later block can confirm a falling background instead.
    int32_t initial_lower = decoder->background_lower;
    uint32_t initial_cycle = decoder->background_cycle;
    int32_t preceding_lower = initial_lower;
    uint32_t preceding_release = decoder->background_cycle;
    for (unsigned offset = 0; offset < decoder->prior_count; offset++) {
        unsigned index = (decoder->delayed_head + decoder->ready_count + offset) %
                         INFRARED_LOOKAHEAD_OBSERVATIONS;
        InfraredDelayedObservation *prior = &decoder->delayed[index];
        const InfraredObservation *raw = &prior->observation;
        uint32_t release = raw->end_cycle - raw->duration;
        uint32_t elapsed = release - preceding_release;
        int32_t raw_lower = ((int32_t)raw->signal_duration - CAPTURE_TIMESTAMP_TRAIL_CYCLES - raw->sample_gap_cycles) * BACKGROUND_SCALE;
        preceding_lower = elapsed > INFRARED_MAX_OBSERVATION_CYCLES * BACKGROUND_SCALE
            ? raw_lower : preceding_lower - (int32_t)elapsed;
        int32_t ceiling = initial_lower + (int32_t)(release - initial_cycle);
        if (raw_lower > preceding_lower) {
            preceding_lower = raw_lower < ceiling ? raw_lower : ceiling;
        }
        prior->preceding_lower = preceding_lower;
        preceding_release = release;
    }
    decoder->background_lower = preceding_lower;
    decoder->background_cycle = preceding_release;

    uint32_t best_key = later(future_key, decoder->block_future_key)
        ? future_key : decoder->block_future_key;
    int32_t following_lower = 0;
    uint32_t following_release = 0;
    all_quiet = decoder->prior_count && !decoder->ready_count;
    unsigned quiet_count = 0;
    for (unsigned remaining = decoder->prior_count; remaining; remaining--) {
        unsigned index = (decoder->delayed_head + decoder->ready_count + remaining - 1u) %
                         INFRARED_LOOKAHEAD_OBSERVATIONS;
        InfraredDelayedObservation *prior = &decoder->delayed[index];
        const InfraredObservation *raw = &prior->observation;
        uint32_t release = raw->end_cycle - raw->duration;
        int32_t propagated = remaining == decoder->prior_count
            ? (int32_t)(best_key + release)
            : following_lower - (int32_t)(following_release - release);
        int32_t measured_upper = ((int32_t)signal_upper(decoder, raw) - CAPTURE_TIMESTAMP_TRAIL_CYCLES) * BACKGROUND_SCALE;
        int32_t raw_lower = ((int32_t)raw->signal_duration - CAPTURE_TIMESTAMP_TRAIL_CYCLES - raw->sample_gap_cycles) * BACKGROUND_SCALE;
        following_lower = raw_lower > propagated ? raw_lower : propagated;
        following_release = release;
        if (following_lower > prior->preceding_lower) {
            prior->preceding_lower = following_lower;
        }
        int32_t ceiling = initial_lower + (int32_t)(release - initial_cycle);
        if (prior->preceding_lower > ceiling) prior->preceding_lower = ceiling;
        if (prior->preceding_lower > measured_upper + input_variation) {
            quiet_count = 0;
            all_quiet = false;
        } else {
            quiet_count++;
        }
        prior->quiet_count = quiet_count;
    }
prepared:
    decoder->background_q8 = decoder->background_lower > 0
        ? (uint32_t)decoder->background_lower / (BACKGROUND_SCALE / 256) : 0;
    decoder->ready_count += decoder->prior_count;
    decoder->prior_count = decoder->current_count;
    decoder->prior_upper_minimum = decoder->block_upper_minimum;
    decoder->prior_lower_maximum = decoder->block_lower_maximum;
    decoder->prior_past_key = decoder->block_past_key;
    decoder->current_count = 0;
    if (all_quiet) {
        decoder->delayed_head = (decoder->delayed_head + decoder->ready_count - 1u) %
                               INFRARED_LOOKAHEAD_OBSERVATIONS;
        decoder->ready_count = 1;
    }
}

static inline __attribute__((always_inline)) bool observe(InfraredDecoder *decoder,
                                                  const InfraredObservation *observation,
                                                  InfraredDecodedByte *byte)
{
    if (__builtin_expect(observation->flags != 0, 0)) {
        if (observation->flags & INFRARED_OBSERVATION_RECONFIGURE) {
            reset_decoder(decoder);
        } else if (observation->flags & INFRARED_OBSERVATION_REARM) {
            infrared_decoder_rearm(decoder);
        }
    }
    uint32_t release = observation->end_cycle - observation->duration;
    uint32_t unseen = release - decoder->previous_end;
    if (__builtin_expect((observation->flags & INFRARED_ERROR_ACQUISITION_GAP) ||
        observation->duration < CAPTURE_TIMESTAMP_TRAIL_CYCLES ||
        observation->duration > INFRARED_MAX_OBSERVATION_CYCLES || !observation->sample_gap_cycles ||
        (decoder->previous_end && unseen >= INFRARED_MIN_PULSE_CYCLES), 0)) {
        return reject(decoder, observation->end_cycle, INFRARED_ERROR_ACQUISITION_GAP, byte);
    }
    decoder->previous_end = observation->end_cycle;
    if (observation->floating_cycles && (!decoder->floating_minimum ||
        observation->floating_cycles < decoder->floating_minimum)) {
        decoder->floating_minimum = observation->floating_cycles;
    }
    if (__builtin_expect(!decoder->background_ready, 0)) {
        acquire_background(decoder, observation);
        return false;
    }
    int32_t raw_lower = ((int32_t)observation->signal_duration - CAPTURE_TIMESTAMP_TRAIL_CYCLES - observation->sample_gap_cycles) *
                        BACKGROUND_SCALE;
    uint32_t future_key = (uint32_t)raw_lower - release;
    if (decoder->current_count && release - decoder->block_start >= LOOKAHEAD_CYCLES) {
        resolve_prior_block(decoder, future_key, (int32_t)observation->signal_duration - observation->sample_gap_cycles);
    }
    unsigned queued = decoder->ready_count + decoder->prior_count + decoder->current_count;
    if (queued == INFRARED_LOOKAHEAD_OBSERVATIONS) {
        return reject(decoder, observation->end_cycle, INFRARED_ERROR_OVERRUN, byte);
    }
    unsigned tail = (decoder->delayed_head + queued) % INFRARED_LOOKAHEAD_OBSERVATIONS;
    decoder->delayed[tail].observation = *observation;
    uint32_t past_key = (uint32_t)raw_lower + release;
    if (!decoder->current_count) {
        decoder->block_upper_minimum = observation->signal_duration;
        decoder->block_lower_maximum = (int16_t)observation->signal_duration - observation->sample_gap_cycles;
        decoder->block_gap = observation->sample_gap_cycles;
        decoder->block_uniform = true;
        decoder->block_past_key = past_key;
        decoder->block_start = release;
        decoder->block_future_key = future_key;
    } else if (decoder->block_uniform && observation->signal_duration == decoder->block_upper_minimum &&
               observation->sample_gap_cycles == decoder->block_gap) {
        decoder->block_past_key = past_key;
    } else {
        decoder->block_uniform = false;
        if (observation->signal_duration < decoder->block_upper_minimum) {
            decoder->block_upper_minimum = observation->signal_duration;
        }
        int16_t lower_duration = (int16_t)observation->signal_duration - observation->sample_gap_cycles;
        if (lower_duration > decoder->block_lower_maximum) {
            decoder->block_lower_maximum = lower_duration;
        }
        if (later(past_key, decoder->block_past_key)) {
            decoder->block_past_key = past_key;
        }
        if (later(future_key, decoder->block_future_key)) {
            decoder->block_future_key = future_key;
        }
    }
    decoder->current_count++;

    while (decoder->ready_count) {
        unsigned quiet_count = decoder->delayed[decoder->delayed_head].quiet_count;
        unsigned retired = quiet_count ? quiet_count : 1u;
        unsigned last = (decoder->delayed_head + retired - 1u) % INFRARED_LOOKAHEAD_OBSERVATIONS;
        InfraredObservation ready = decoder->delayed[last].observation;
        decoder->delayed_head = (decoder->delayed_head + retired) % INFRARED_LOOKAHEAD_OBSERVATIONS;
        decoder->ready_count -= retired;
        decoder->processed_cycle = ready.end_cycle;
        if (decode_observation(decoder, &ready, quiet_count == 0, byte)) {
            return true;
        }
    }
    return false;
}

bool INFRARED_FAST_CODE infrared_decoder_consume(InfraredDecoder *decoder,
                                                 const InfraredObservation *observations,
                                                 unsigned count, unsigned *consumed,
                                                 InfraredDecodedByte *byte)
{
    unsigned index = 0;
    bool emitted = false;
    while (index < count) {
        emitted = observe(decoder, &observations[index++], byte);
        if (emitted) {
            break;
        }
    }
    *consumed = index;
    return emitted;
}

size_t infrared_encode_frame(const uint8_t *bytes, size_t length, uint32_t *symbols,
                             size_t capacity, uint32_t *last_pulse_tick, uint32_t *total_ticks) {
    if (!bytes || !length || length > INFRARED_MAX_FRAME || capacity < length * 10u) {
        return 0;
    }

    size_t symbol_count = 0;
    uint32_t elapsed_ticks = 0;
    uint32_t fractional_ticks = 0;
    uint32_t last_light = 0;

    for (size_t byte_index = 0; byte_index < length; byte_index++) {
        uint32_t serial_bits = ((uint32_t)(bytes[byte_index] ^ 0xAAu) << 1) | (1u << 9);
        for (unsigned bit_index = 0; bit_index < 10; bit_index++) {
            /* 80 MHz / 115,200 = 694 + 4/9 ticks. */
            uint32_t bit_ticks = 694;
            fractional_ticks += 4;
            if (fractional_ticks >= 9) {
                fractional_ticks -= 9;
                bit_ticks++;
            }

            if ((serial_bits & (1u << bit_index)) == 0) {
                symbols[symbol_count++] = 130u | (1u << 15) | ((bit_ticks - 130u) << 16);
                last_light = elapsed_ticks + 130u;
            } else {
                uint32_t first_half = bit_ticks / 2;
                symbols[symbol_count++] = first_half | ((bit_ticks - first_half) << 16);
            }
            elapsed_ticks += bit_ticks;
        }
    }

    if (last_pulse_tick) {
        *last_pulse_tick = last_light;
    }
    if (total_ticks) {
        *total_ticks = elapsed_ticks;
    }
    return symbol_count;
}
