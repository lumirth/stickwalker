/* Electrical input to the production decoder. The full firmware journeys
 * execute the Xtensa sampler; this faster sweep explores the circuit separately. */
#include "infrared_decoder.h"
#include "infrared_calibration.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

enum { PAYLOAD = 136, CALIBRATION = 4096, MAX_PULSES = 2048 };
typedef struct { double on, off; } Pulse;
typedef struct {
    double capacitance, resistance, trip, ambient, signal, bit, width;
    unsigned seed, stalls;
    // Room light can change between calibration and a transfer, and during it.
    double shift, shift_start, shift_cycles;
    // After a strong flash the diode keeps conducting briefly: a fraction of
    // the flash's current that decays with this time constant, in cycles.
    double tail, tail_cycles;
    // Set when the tail alone keeps the pin saturated until the next bit.
    bool undecodable;
    Pulse pulses[MAX_PULSES];
    unsigned count, cursor;
    double now, traffic_start;
} Channel;

static unsigned random_word(Channel *c)
{
    c->seed ^= c->seed << 13;
    c->seed ^= c->seed >> 17;
    c->seed ^= c->seed << 5;
    return c->seed;
}

static double ambient_at(const Channel *c, double at)
{
    // Room-light ripple plus a hand moving into and then out of the light.
    double ramp = fmax(0, 1 - fabs((at - c->traffic_start) / 480000 - 1));
    double drift = c->shift_cycles ? fmin(1, fmax(0, (at - c->shift_start) / c->shift_cycles)) : 0;
    return c->ambient * (1 + c->shift * drift) + (c->ambient >= 15 ? 2.5 : 0.3) * (1 + sin(at * (6.28318530718 * 120 / 240000000)))
        + 0.4 * ramp;
}

static double discharge(const Channel *c, double v, double cycles, double current, bool pull)
{
    double ns = cycles * (1000.0 / 240);
    if (!pull) return v - current * ns / (c->capacitance * 1000);
    double offset = current * c->resistance / 1000;
    return (v + offset) * exp(-ns / (c->resistance * c->capacitance)) - offset;
}

static double crossing(const Channel *c, double v, double current, bool pull, double trip)
{
    if (v <= trip) return 0;
    if (!pull) return (v - trip) * c->capacitance * 240 / current;
    double offset = current * c->resistance / 1000;
    return log((v + offset) / (trip + offset)) * c->resistance * c->capacitance * 0.24;
}

static InfraredObservation measure(Channel *c, unsigned blocks)
{
    double release = c->now;
    unsigned stall = c->stalls && random_word(c) % 17 == 0 ? random_word(c) % 31 : 0;
    double pull_at = release + (blocks ? blocks * 36 + 18 : 13) + stall;
    double trip = c->trip + ((int)(random_word(c) % 101) - 50) * 0.0001;
    double at = release, voltage = 3.3, crossed = release + 1440;
    while (c->cursor < c->count && c->pulses[c->cursor].off <= at) c->cursor++;
    unsigned cursor = c->cursor;
    while (at < release + 1440) {
        bool pull = at >= pull_at;
        double end = pull ? release + 1440 : pull_at;
        double current = ambient_at(c, at);
        if (cursor < c->count) {
            const Pulse *p = &c->pulses[cursor];
            if (at < p->on) end = fmin(end, p->on);
            else { current += c->signal; end = fmin(end, p->off); }
        }
        if (c->tail && cursor && at >= c->pulses[cursor - 1].off) {
            // Short steps keep the decaying tail close to piecewise constant.
            end = fmin(end, at + c->tail_cycles / 8);
            double since = at + (end - at) / 2 - c->pulses[cursor - 1].off;
            current += c->signal * c->tail * exp(-since / c->tail_cycles);
        }
        double delta = crossing(c, voltage, current, pull, trip);
        if (at + delta <= end) { crossed = at + delta; break; }
        voltage = discharge(c, voltage, end - at, current, pull);
        at = end;
        if (cursor < c->count && at >= c->pulses[cursor].off) cursor++;
    }
    double last_float = pull_at - 18 - stall;
    bool pulled = !blocks || crossed > last_float;
    double first = pulled ? pull_at + 8 : release + 8;
    double read = first + 2 * ceil(fmax(0, crossed - first) / 2);
    unsigned gap = pulled && read == first ? (unsigned)(first - (blocks ? last_float : release)) : 2;
    unsigned duration = (unsigned)llround(read + 4 - release);
    unsigned floating = pulled ? (unsigned)llround(pull_at - release) : 0;
    InfraredObservation sample = {
        .end_cycle = (uint32_t)llround(read + 4), .duration = duration,
        .sample_gap_cycles = gap,
        .signal_duration = duration - floating + (pulled ? blocks * 36 - (blocks ? 2 : 0) : 0),
        .floating_cycles = floating
    };
    c->now = fmax(release + 224, read + 4 + 52 + (c->stalls && random_word(c) % 17 == 0 ? random_word(c) % 31 : 0));
    return sample;
}

static void send(Channel *c, const unsigned char *bytes, unsigned count, double start)
{
    for (unsigned i = 0; i < count; i++) {
        unsigned serial = 0x200 | (bytes[i] << 1);
        for (unsigned bit = 0; bit < 10; bit++) if (!(serial & (1u << bit))) {
            if (c->count == MAX_PULSES) abort();
            double on = start + (10 * i + bit) * c->bit + (int)(random_word(c) % 49) - 24;
            c->pulses[c->count++] = (Pulse){on, on + c->width};
        }
    }
}

static bool run(Channel c, unsigned number)
{
    unsigned char bytes[PAYLOAD];
    for (unsigned i = 0; i < PAYLOAD; i++) bytes[i] = (unsigned char)(i * 73 + number);
    c.now = number & 8 ? (double)UINT32_MAX - 3000000 : 10000;
    c.traffic_start = 1e12;
    // A peer probe lands during startup, independently of calibration windows.
    unsigned char probe = 0x56;
    send(&c, &probe, 1, c.now + 400000);
    InfraredCalibration calibration = {.floating_blocks = INFRARED_INITIAL_FLOAT_BLOCKS};
    while (!calibration.ready) {
        unsigned longest = 0;
        for (unsigned i = 0; i < 8; i++) {
            InfraredObservation o = measure(&c, calibration.floating_blocks);
            if (o.duration > longest) longest = o.duration;
        }
        infrared_calibration_observe(&calibration, longest);
    }
    InfraredObservation startup[CALIBRATION];
    for (unsigned i = 0; i < CALIBRATION; i++) startup[i] = measure(&c, calibration.floating_blocks);
    InfraredDecoder decoder;
    infrared_decoder_init(&decoder);
    infrared_decoder_measure_noise(&decoder, startup, CALIBRATION);
    for (unsigned at = 0; at < CALIBRATION;) {
        unsigned consumed;
        InfraredDecodedByte byte;
        infrared_decoder_consume(&decoder, startup + at, CALIBRATION - at, &consumed, &byte);
        at += consumed;
    }
    infrared_decoder_rearm(&decoder);
    c.count = c.cursor = 0;
    c.traffic_start = c.now + 60000 + number * 73 % 2083;
    if (c.shift_cycles == 120000) {
        // A lamp or a shadow settles a few milliseconds before the reply.
        c.shift_start = c.now;
        c.traffic_start += 1000000;
    } else if (c.shift_cycles) {
        c.shift_start = c.traffic_start;
    }
    send(&c, bytes, PAYLOAD, c.traffic_start);
    double end = c.traffic_start + PAYLOAD * 10 * c.bit + 300000;
    unsigned received = 0, errors = 0, wrong = 0;
    while (c.now < end) {
        InfraredObservation o = measure(&c, calibration.floating_blocks);
        InfraredDecodedByte byte;
        unsigned consumed;
        if (infrared_decoder_consume(&decoder, &o, 1, &consumed, &byte)) {
            // The startup probe can land after calibration. The packet layer
            // separates it and other idle bursts from the payload.
            if (c.now < c.traffic_start) continue;
            if (byte.errors || received >= PAYLOAD || byte.value != (bytes[received] ^ 0xaa)) errors++;
            if (!byte.errors && received < PAYLOAD && byte.value != (bytes[received] ^ 0xaa)) wrong++;
            received++;
        }
    }
    if (received == PAYLOAD && !errors) return true;
    if (c.undecodable) {
        // The pin carries no information here; a wrong byte the decoder did
        // not flag would be the failure that matters.
        return !wrong;
    }
    printf("channel %u: C=%.0f R=%.0f trip=%.3f ambient=%.1f%+.0f%%%s signal=%.1f bit=%.0f stalls=%u: %u/%u bytes, %u errors (allowance=%u float=%u)\n",
           number, c.capacitance, c.resistance, c.trip, c.ambient, c.shift * 100,
           c.shift_cycles == 120000 ? " before" : c.shift_cycles ? " during" : "", c.signal, c.bit, c.stalls,
           received, PAYLOAD, errors, decoder.jitter_allowance_q8 / 256, calibration.floating_blocks);
    return false;
}

int main(void)
{
    const double capacitance[] = {10, 19, 50}, resistance[] = {30, 45, 80};
    const double trip[] = {1.2, 1.55, 2.0}, signal[] = {28.5, 100, 500};
    unsigned cases = 0, failures = 0, undecodable = 0;
    for (unsigned c = 0; c < 3; c++) for (unsigned r = 0; r < 3; r++)
    for (unsigned t = 0; t < 3; t++) for (unsigned s = 0; s < 3; s++)
    for (unsigned environment = 0; environment < 8; environment++) {
        Channel channel = {.capacitance = capacitance[c], .resistance = resistance[r],
            .trip = trip[t], .signal = signal[s], .ambient = (double[]){0.1, 1.5, 15, 20}[environment / 2],
            .bit = environment & 1 ? 2100 : 2065, .width = environment & 1 ? 390 : 350,
            .stalls = environment & 1, .seed = 0x9e3779b9u + cases};
        if (!run(channel, cases++)) failures++;
        const double changes[][2] = {{0.1, 120000}, {-0.1, 120000}, {0.2, 3000000}, {-0.2, 3000000}};
        for (unsigned k = 0; k < 4; k++) {
            Channel changed = channel;
            changed.shift = changes[k][0];
            changed.shift_cycles = changes[k][1];
            changed.seed ^= 0x5a5a + k;
            if (!run(changed, cases++)) failures++;
        }
    }
    // Exercise the sensitivity paths separately from the independent component
    // extremes above: the same received current does not yield the same voltage
    // signal for every possible C/R/input-threshold combination.
    for (unsigned phase = 0; phase < 256; phase++) {
        Channel weak = {.capacitance = 19, .resistance = 45, .trip = 1.55,
            .ambient = phase & 1 ? 1.5 : 0.1, .signal = 3.5,
            .bit = (double[]){2065, 2071, 2083, 2100}[(phase / 2) % 4], .width = phase & 1 ? 390 : 350,
            .stalls = 1, .seed = 0x91e10da5u + phase};
        if (!run(weak, cases++)) failures++;
    }
    // A flash bright enough to saturate the pin, followed by the diode's
    // recovery tail. Fitted to a close-range DS capture (next measurement about
    // 90 cycles short, the one after about 10), the tail starts near 17% of a
    // 160 uA flash and decays with a 300-cycle time constant. Sweep half to
    // twice that share and two thirds to one and a half times that decay, for
    // flashes up to sixty times as strong.
    for (unsigned strength = 0; strength < 4; strength++)
    for (unsigned share = 0; share < 3; share++)
    for (unsigned length = 0; length < 3; length++)
    for (unsigned variant = 0; variant < 4; variant++) {
        Channel strong = {.capacitance = 19, .resistance = 45, .trip = 1.55,
            .ambient = variant & 1 ? 1.5 : 0.1, .signal = (double[]){160, 500, 2000, 10000}[strength],
            .tail = (double[]){0.08, 0.17, 0.34}[share], .tail_cycles = (double[]){200, 300, 450}[length],
            .bit = variant & 2 ? 2100 : 2065, .width = variant & 1 ? 390 : 350,
            .stalls = 1, .seed = 0x7f4a7c15u + cases};
        // The current that ends a 12-block floating phase by itself, in uA.
        double saturating = strong.capacitance * (3.3 - strong.trip) * 240 / ((12 * 36 + 18) * 1000) * 1000;
        double tail_start = strong.signal * strong.tail;
        strong.undecodable = tail_start > saturating &&
            strong.tail_cycles * log(tail_start / saturating) + strong.width > strong.bit - 26;
        undecodable += strong.undecodable;
        if (!run(strong, cases++)) failures++;
    }
    printf("Optical circuit sweep: %u cases, %u failed (%u physically undecodable, checked for false bytes)\n",
           cases, failures, undecodable);
    if (getenv("STICK_RANGE")) {
        // IrDA SIR transmitters emit 40 to 500 mW/sr; the BPV10NF yields about
        // 55 uA per mW/cm2. Irradiance falls with the square of distance.
        const double intensity[] = {40, 500}, distance[] = {0.5, 1, 2, 5, 10, 20, 30, 50, 70, 100};
        for (unsigned i = 0; i < 2; i++) {
            printf("%3.0f mW/sr:", intensity[i]);
            for (unsigned d = 0; d < sizeof(distance) / sizeof(*distance); d++) {
                double current = intensity[i] * 55 / (distance[d] * distance[d]);
                unsigned passed = 0;
                for (unsigned phase = 0; phase < 16; phase++) {
                    Channel link = {.capacitance = 19, .resistance = 45, .trip = 1.55,
                        .ambient = phase & 1 ? 1.5 : 0.1, .signal = current,
                        .tail = current > 50 ? 0.04 : 0, .tail_cycles = 300,
                        .bit = (double[]){2065, 2083, 2100, 2071}[phase % 4], .width = phase & 2 ? 390 : 350,
                        .stalls = 1, .seed = 0x2545f491u + phase * 977 + d};
                    passed += run(link, 100000 + phase);
                }
                printf(" %gcm:%u/16", distance[d], passed);
            }
            printf("\n");
        }
    }
    return failures != 0;
}
