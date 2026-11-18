#ifndef STICKWALKER_INFRARED_CALIBRATION_H
#define STICKWALKER_INFRARED_CALIBRATION_H

#include <stdbool.h>
#include <stdint.h>

#include "infrared_timing.h"

typedef struct {
    uint32_t floating_blocks;
    unsigned settled_batches;
    bool ready;
} InfraredCalibration;

static inline __attribute__((always_inline)) void
infrared_calibration_observe(InfraredCalibration *calibration, uint32_t longest)
{
    if (calibration->ready) return;
    if (longest > INFRARED_DARK_TARGET_CYCLES && calibration->floating_blocks) {
        uint32_t excess = longest - INFRARED_DARK_TARGET_CYCLES;
        uint32_t blocks = (excess + INFRARED_POLL_BLOCK_CYCLES - 1u) /
                          INFRARED_POLL_BLOCK_CYCLES;
        calibration->floating_blocks = blocks < calibration->floating_blocks
            ? calibration->floating_blocks - blocks : 0;
        calibration->settled_batches = 0;
    } else {
        /* A peer pulse may shorten the first batch. Keep observing through a
         * second batch before handing the sampler to its uninterrupted loop. */
        calibration->ready = ++calibration->settled_batches >= 2u;
    }
}

#endif
