# How Stickwalker counts steps

Stickwalker uses the step-counting algorithm reconstructed from the original
Pokéwalker firmware. The Stick supplies acceleration measurements; the
Pokéwalker's own logic decides which movement counts as walking and how many
steps to award.

The original Pokéwalker uses a Bosch BMA150 accelerometer. The Stick S3 uses a
[Bosch BMI270](https://www.bosch-sensortec.com/en/products/motion-sensors/imus/bmi270),
a newer motion sensor designed for wearables. Stickwalker reads its three
acceleration axes through M5Unified and converts them to the signed eight-bit
samples expected by the reconstructed firmware. One g becomes 64 counts,
matching the high byte of the BMA150's ±2 g output. Readings saturate at the
original signed range. If a sensor poll has no fresh reading, the previous
sample is retained.

The reconstructed firmware collects batches of 64 samples at its original
cadence. It analyzes repeated movement across all three axes with a fixed-point
Fourier transform, selects a walking frequency and carries fractional steps
between batches. The original pacing logic adds those steps to the daily and
total counts and awards Watts.

Keeping that algorithm and matching its input scale gives Stickwalker a basis
for reproducing the original's walking behavior. Sensor filtering, mounting
and how you carry the Stick can affect the measured motion. A side-by-side walk
with an original Pokéwalker is the useful comparison for numerical accuracy.

The sensor adapter is in [`motion_sample.cpp`](../motion_sample.cpp) and
[`accel_bridge.cpp`](../accel_bridge.cpp). The reconstructed algorithm is in
[`pw_fourier.c`](../../src/application/pw_fourier.c),
[`pw_pedometer.c`](../../src/application/pw_pedometer.c) and
[`pw_power.c`](../../src/application/pw_power.c).
