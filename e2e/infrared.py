#!/usr/bin/env python3
"""Replay measured GPIO5 samples through the production C decoder."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / 'main/infrared/infrared_esp32.c').read_text()
window = re.search(r'^#define NOISE_CALIBRATION_SAMPLES (\d+)u$', source, re.M)
assert window and 0 < int(window[1]) <= 8192, 'Startup window exceeds the capture'

with tempfile.TemporaryDirectory(prefix='stick-infrared-') as directory:
    temp = Path(directory)
    (temp / 'replay.c').write_text(r'''
#include "infrared_decoder.h"
#include <stdio.h>
#include <string.h>

static unsigned read_capture(FILE *file, InfraredObservation *out, unsigned limit, bool legacy) {
    if (!legacy) return fread(out, sizeof(*out), limit, file);
    struct { uint32_t end; uint16_t duration; uint8_t flags, gap; } saved;
    unsigned count = 0;
    while (count < limit && fread(&saved, sizeof(saved), 1, file) == 1) {
        out[count++] = (InfraredObservation){.end_cycle=saved.end,
            .duration=saved.duration, .flags=saved.flags, .sample_gap_cycles=saved.gap,
            .signal_duration=saved.duration};
    }
    return count;
}

int main(int argc, char **argv) {
    InfraredObservation observations[8192];
    unsigned total_failed = 0;
    InfraredObservation calibration[8192];
    FILE *file;
    for (unsigned fixture = 0; fixture < 4; fixture++) {
        bool legacy = fixture == 0;
        const char *path = argv[legacy ? 1 : fixture + 2];
        file = fopen(path, "rb");
        if (!file || read_capture(file, observations, 8192, legacy) != 8192) return 2;
        fclose(file);
        unsigned failed = 0, windows = 0, false_bytes = 0, errors = 0;
        // Startup can land anywhere in the measured quiet waveform. Every window
        // must reject the entire quiet capture, not just the samples used to fit it.
        for (int start = -64; start + CALIBRATION_SAMPLES <= 8192; start += 64) {
            InfraredDecoder decoder;
            infrared_decoder_init(&decoder);
            // Also exercise timed captures with the default allowance. The last
            // capture follows a quieter startup, so never fit it to itself here.
            if (legacy && start < 0) {
                memcpy(calibration, observations, sizeof(observations));
                // Peer light in the startup window must not discard all quiet evidence.
                for (unsigned i = 1024; i < 1032; i++) calibration[i].signal_duration -= 100;
                infrared_decoder_measure_noise(&decoder, calibration, CALIBRATION_SAMPLES);
            }
            if (fixture == 3 && start < 0) {
                FILE *startup = fopen(argv[3], "rb");
                if (!startup || read_capture(startup, calibration, 8192, false) != 8192) return 2;
                fclose(startup);
                infrared_decoder_measure_noise(&decoder, calibration, CALIBRATION_SAMPLES);
            }
            if (start >= 0) infrared_decoder_measure_noise(&decoder, observations + start, CALIBRATION_SAMPLES);
            unsigned at = 0, bytes = 0, damaged = 0;
            while (at < 8192) {
                InfraredDecodedByte byte;
                unsigned consumed;
                if (infrared_decoder_consume(&decoder, observations + at, 8192 - at, &consumed, &byte)) {
                    if (byte.errors) damaged++; else bytes++;
                }
                at += consumed;
            }
            windows++;
            failed += bytes != 0 || damaged != 0;
            false_bytes += bytes;
            errors += damaged;
        }
        printf("%s: %u cases, %u failed, %u false bytes, %u errors\n",
               path, windows, failed, false_bytes, errors);
        total_failed += failed;
    }
    file = fopen(argv[2], "rb");
    if (!file) return 2;
    unsigned count = read_capture(file, observations, 8192, true);
    fclose(file);
    if (!count) return 2;
    InfraredDecoder decoder;
    infrared_decoder_init(&decoder);
    decoder.jitter_allowance_q8 = 23 * 256; // Measured on the physical transfer.
    // The crop begins inside an already calibrated transfer. Establish its
    // measured quiet baseline through the normal acquisition path first.
    for (unsigned i = 0; i < 64; i++) {
        InfraredObservation quiet = {.end_cycle = observations[0].end_cycle - 400 * (64 - i),
                                      .duration = 356, .sample_gap_cycles = 2, .signal_duration = 356};
        InfraredDecodedByte byte;
        unsigned consumed;
        infrared_decoder_consume(&decoder, &quiet, 1, &consumed, &byte);
    }
    infrared_decoder_rearm(&decoder);
    const unsigned expected[] = {0x67, 0x59};
    unsigned at = 0, decoded = 0, damaged = 0;
    while (at < count) {
        InfraredDecodedByte byte;
        unsigned consumed;
        if (infrared_decoder_consume(&decoder, observations + at, count - at, &consumed, &byte)) {
            if (byte.errors || decoded >= 2 || byte.value != expected[decoded]) damaged++;
            decoded++;
        }
        at += consumed;
    }
    printf("Split-pulse capture: %u of 2 bytes, %u errors\n", decoded, damaged);
    return total_failed || decoded != 2 || damaged ? 1 : 0;
}
''')
    binary = temp / 'replay'
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra',
                    '-Wno-unused-parameter', '-DCALIBRATION_SAMPLES=' + window[1],
                    '-I', str(root / 'main/infrared'), '-I', str(root / 'pokewalker/include'),
                    str(temp / 'replay.c'), str(root / 'main/infrared/infrared_decoder.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(root / 'e2e/fixtures/gpio5-quiet.bin'),
                    str(root / 'e2e/fixtures/gpio5-split-pulse.bin'),
                    str(root / 'e2e/fixtures/gpio5-timed-quiet.bin'),
                    str(root / 'e2e/fixtures/gpio5-timed-bus-load.bin'),
                    str(root / 'e2e/fixtures/gpio5-post-transfer-quiet.bin')], check=True)

    channel = temp / 'optical-channel'
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'main/infrared'), '-I', str(root / 'pokewalker/include'),
                    str(root / 'e2e/optical_channel.c'),
                    str(root / 'main/infrared/infrared_decoder.c'), '-lm', '-o', str(channel)], check=True)
    subprocess.run([str(channel)], check=True)
