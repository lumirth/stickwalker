# How Stickwalker counts steps

Stickwalker runs the Pokéwalker's original step-counting algorithm. The Stick's
accelerometer measures movement, and the game code decides which movement
counts as walking and how many steps to award.

The Pokéwalker uses a Bosch BMA150 accelerometer. The Stick S3 uses a
[Bosch BMI270](https://www.bosch-sensortec.com/en/products/motion-sensors/imus/bmi270),
a newer sensor designed for wearables. Stickwalker reads all three axes through
M5Unified and converts them to the signed eight-bit values used by the original
firmware. One g becomes 64 counts, matching the BMA150's ±2 g output after the
original firmware takes its high byte. Values beyond that range saturate.
Between fresh sensor readings, the adapter keeps the previous sample.

The game collects 64 samples at its original cadence, then uses a fixed-point
Fourier transform to find repeated movement across the three axes. It selects
a walking frequency and carries fractional steps into the next batch. The
original pacing logic adds steps to the daily and lifetime totals and awards
Watts.

Matching the input scale and timing lets the original algorithm work with the
new sensor, so Stickwalker should count steps much like a Pokéwalker. As with
the original device, how you carry it affects the motion it measures.

For the sensor conversion, see [`motion_sample.cpp`](../motion_sample.cpp) and
[`accel_bridge.cpp`](../accel_bridge.cpp). The step algorithm is in
[`pw_fourier.c`](../../src/application/pw_fourier.c) and
[`pw_pedometer.c`](../../src/application/pw_pedometer.c).
[`pw_power.c`](../../src/application/pw_power.c) controls the sampling schedule.
