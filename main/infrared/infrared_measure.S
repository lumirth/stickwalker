#include "infrared_timing.h"
#include "infrared_capture.h"

    .equ GPIO5_MUX_REGISTER, 0x60009018
    .equ GPIO_OUTPUT_ENABLE_CLEAR, 0x60004028
    .equ GPIO_OUTPUT_ENABLE_SET, 0x60004024
    .equ GPIO_PULL_DOWN, 128

    .section .iram1,"ax"
    .literal_position
    .align 4
    .global infrared_capture_batch
    .type infrared_capture_batch,@function

infrared_capture_batch:
    entry a1, 48
    mov a12, a2
    mov a11, a3
    movi a13, INFRARED_CAPTURE_BATCH
    l32i a2, a11, CAPTURE_FLOATING_MUX_OFFSET
    l32i a14, a11, CAPTURE_FLOATING_READS_OFFSET
    l32i a15, a11, CAPTURE_PULLED_READS_OFFSET
    l32i a8, a11, CAPTURE_PREVIOUS_RELEASE_OFFSET

.Lnext_measurement:
    l32i a14, a11, CAPTURE_FLOATING_READS_OFFSET
    addi a3, a2, GPIO_PULL_DOWN - 1
    addi a4, a14, -1
    addi a5, a15, -1
    beqz a8, .Lprepare_node
.Lminimum_period:
    rsr.ccount a6
    sub a6, a6, a8
    movi a7, INFRARED_MIN_SAMPLE_CYCLES
    bltu a6, a7, .Lminimum_period
.Lprepare_node:
    movi a6, GPIO5_MUX_REGISTER
    s32i a2, a6, 0
    movi a9, GPIO_OUTPUT_ENABLE_CLEAR
    movi a10, 32
    rsr.ccount a7
    s32i a10, a9, 0
    memw
    rsr.ccount a8
    _beqz a14, .Ldirect_pull

    j .Lfloat_poll_aligned
    .p2align 4
.Lfloat_poll_aligned:
    ee.get_gpio_in a10
    bbci a10, 0, .Lfirst_floating_crossing
    loop a4, .Lfloating_end
.Lfloating:
    bbci a10, 0, .Lfloating_crossing
    ee.get_gpio_in a10
.Lfloating_end:
    bbci a10, 0, .Lfloating_crossing
    j .Lfinal_float_read

.Lfirst_floating_crossing:
    rsr.ccount a9
    movi a14, 0
    sub a10, a9, a7
    addi a10, a10, -4
    j .Lrecharge

.Lfloating_crossing:
    rsr.ccount a9
    rsr.lcount a10
    sub a10, a14, a10
    slli a10, a10, 1
    sub a4, a9, a7
    sub a10, a4, a10
    addi a10, a10, 1
    movi a14, 0
    j .Lminimum_gap

.Ldirect_pull:
    addi a3, a3, 1
    j .Lpull_down

.Lfinal_float_read:
    addi a3, a3, 1
    ee.get_gpio_in a10
    bbci a10, 0, .Ltransition_crossing
    rsr.ccount a7
.Lpull_down:
    s32i a3, a6, 0
    memw
    rsr.ccount a14
    sub a14, a14, a8
    j .Lpulled_poll_aligned
    .p2align 4
.Lpulled_poll_aligned:
    ee.get_gpio_in a10
    bbci a10, 0, .Lfirst_pulled_crossing
    rsr.ccount a4
    loop a5, .Lpulled_end
.Lpulled:
    bbci a10, 0, .Lpulled_crossing
    ee.get_gpio_in a10
.Lpulled_end:
    bbci a10, 0, .Lpulled_crossing
    nop.n
    nop.n
    rsr.ccount a9
    movi a10, 4
    slli a10, a10, 16
    j .Lrecharge_packed

.Lfirst_pulled_crossing:
    rsr.ccount a9
    sub a10, a9, a7
    l32i a3, a11, CAPTURE_FLOATING_READS_OFFSET
    beqz a3, .Lfirst_direct_crossing
    addi a10, a10, -2
    j .Lrecharge
.Lfirst_direct_crossing:
    addi a10, a10, -4
    j .Lrecharge

/* A two-instruction polling loop is a lower bound, not a fixed elapsed time.
 * Count completed iterations and retain any extra elapsed cycles in the
 * crossing interval instead of interpreting instruction stalls as darkness. */
.Lpulled_crossing:
    rsr.ccount a9
    rsr.lcount a10
    sub a10, a15, a10
    slli a10, a10, 1
    sub a4, a9, a4
    sub a10, a4, a10
    addi a10, a10, 1
.Lminimum_gap:
    movi a4, 2
    bge a10, a4, .Lbounded_gap
    mov a10, a4
.Lbounded_gap:
    movi a4, 255
    bge a4, a10, .Lrecharge
    movi a10, 4
    slli a10, a10, 16
    j .Lrecharge_packed

.Ltransition_crossing:
    rsr.ccount a9
    movi a14, 0
    movi a10, 5

.Lrecharge:
    slli a10, a10, 24
.Lrecharge_packed:
    movi a4, GPIO_OUTPUT_ENABLE_SET
    movi a5, 32
    s32i a5, a4, 0
    s32i a9, a12, 0
    sub a9, a9, a8
    extui a9, a9, 0, 16
    or a9, a9, a10
    s32i a9, a12, 4
/* Keep optical onset timestamps raw. For light/background comparison only,
 * remove the measured floating-phase delay and add its nominal loop budget. */
    mov a4, a9
    beqz a14, .Lsignal_duration
    sub a4, a4, a14
    l32i a5, a11, CAPTURE_FLOATING_READS_OFFSET
    slli a5, a5, 1
    add a4, a4, a5
.Lsignal_duration:
    extui a4, a4, 0, 16
    slli a5, a14, 16
    or a4, a4, a5
    s32i a4, a12, 8
    addi a12, a12, 12
    addi a13, a13, -1
    bnez a13, .Lnext_measurement

    l32i a3, a11, CAPTURE_STREAM_OFFSET
    beqz a3, .Lreturn
    l32i a4, a3, STREAM_WRITE_COUNT_OFFSET
    l32i a5, a4, 0
    addi a5, a5, INFRARED_CAPTURE_BATCH
    memw
    s32i a5, a4, 0

    l32i a4, a3, STREAM_REQUESTED_MODE_OFFSET
    l32i a9, a4, 0
    bnei a9, INFRARED_CAPTURE_LISTEN_MODE, .Lreturn
    l32i a4, a3, STREAM_READ_COUNT_OFFSET
    l32i a9, a4, 0
    memw
    sub a9, a5, a9
    movi a4, INFRARED_CAPTURE_RING_CAPACITY - INFRARED_CAPTURE_BATCH
    bltu a4, a9, .Lreturn

    extui a4, a5, 0, INFRARED_CAPTURE_RING_BITS
    slli a9, a4, 1
    add a4, a4, a9
    slli a4, a4, 2
    l32i a12, a3, STREAM_OBSERVATIONS_OFFSET
    add a12, a12, a4
    movi a13, INFRARED_CAPTURE_BATCH
    j .Lnext_measurement

.Lreturn:
    s32i a8, a11, CAPTURE_PREVIOUS_RELEASE_OFFSET
    retw
    .size infrared_capture_batch, .-infrared_capture_batch
