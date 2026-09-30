# Power and foreground ownership

The port preserves the native game's display availability and motion-processing
schedule while sleeping the ESP32-S3 between work deadlines. Battery operation
allows Light-sleep; USB power keeps the processor awake for development and
diagnostics.

## Motion and input

Inactive/activity work runs at 1 Hz; moving/interactive work runs at 16 Hz.
Eligible samples are processed immediately with the native FFT and estimator.
Sound and infrared sessions preserve the native sampling gaps because they own
foreground execution and shared workspace. Ordinary menus and games continue
sampling. See the [motion decision](../../docs/adr/0001-preserve-native-motion-ownership.md).

Awake input polls run at 5 ms. Visible operation caps sleep at 100 ms so L's PMIC
press latch remains usable; the 62.5 ms native deadline usually wakes sooner.
Stick Settings counts as visible even when the underlying game is blank.

Screen-off wake uses physical M alone, held for 500 ms. L/R activity neither
polls the PMIC nor shortens the inactive schedule. A cancelled hold reverses
panel preparation. On return to visible input, the L latch is discarded and all
buttons must be released before navigation. See the [wake decision](../../docs/adr/0002-m-only-dark-wake.md).

## Supplies and sleep

`board_hal.cpp` owns PMIC setup. `peripheral_power.cpp` coordinates shared supply
requirements; display, audio and IR adapters request the rails they need.
`power_sleep.cpp` programs wake sources and restores M/R pads from RTC ownership
to digital GPIO after every sleep return, including rejected entries. Setup
failures veto sleep and use a bounded retry.

The [PMIC button review](pmic-input.md) distinguishes
raw L state/latching from classified-click timing. The SINGLE field's 125 ms
option is not a measured raw-input delay. A GPIO13 classified-click IRQ has not
been qualified as a replacement for polling.

## Battery evidence

Battery percentage is estimated from voltage. The
[hardware records](history/power-and-wake.md) cover sleep, wake and scheduling.
To characterize battery life, measure current and discharge under a stated
use profile.

Measure stationary carry, moving carry and lit interaction at the battery path,
including PMIC, conversions, IMU, display/audio rails, IR boost and retained RAM.
Include the screen's idle-on time and transient peaks in the use profile.
The [release checklist](release.md#device-checks-for-a-stable-release) separates battery,
walking, control and infrared qualification.
