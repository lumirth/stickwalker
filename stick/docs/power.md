# Power management

Stickwalker sleeps the ESP32-S3 between game tasks when running on battery.
USB power keeps the CPU awake so the serial and debugger connections remain
available.

## Sampling and buttons

The original game checks for activity at 1 Hz and samples walking at 16 Hz.
Stickwalker processes each sample as it arrives. Sound and infrared pause motion
sampling while they use the shared working memory; menus and games continue to
sample. The [motion scheduling decision](../../docs/adr/0001-preserve-native-motion-ownership.md)
explains why.

Awake button polls run every 5 ms. While the game or Settings is visible, sleep
is capped at 100 ms so L's PMIC press latch can be read. The 62.5 ms game deadline
usually wakes the CPU sooner.

With the screen off, holding M for 500 ms wakes it. Releasing M early cancels
panel preparation. After wake, the adapter clears L's retained press and waits
for all buttons to be released before navigation resumes. See the
[wake policy](../../docs/adr/0002-m-only-dark-wake.md) and
[L button reference](pmic-input.md).

## Peripherals

`board_hal.cpp` configures the PMIC. The display, sound and infrared adapters
request their supplies through `peripheral_power.cpp`, which switches a shared
supply off after its last user releases it.

`power_sleep.cpp` sets the wake sources and restores M and R to digital GPIO
after each sleep attempt, including rejected entries. Configuration failures
keep the CPU awake until a scheduled retry succeeds.

## Measuring power use

Settings estimates charge percentage from battery voltage. To measure battery
life, record current at the battery connection during stationary carry,
walking and screen-on use. Include time spent with the screen lit, sound,
infrared transfers and transient peaks. This measures the whole device,
including the PMIC, sensors and peripheral supplies.

The [device test records](history/power-and-wake.md) contain sleep and wake
results. The [release checklist](release.md#device-checks-for-a-stable-release)
lists the battery tests for a release.
