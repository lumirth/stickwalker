# Use M to wake the screen

Holding physical M through native Center qualification wakes the Stick in every
layout and rotation, roughly half a second. The native firmware also retains
its social-offer opportunity; the adapter no longer starts interactive mode
from a separate 500 ms wall-clock threshold.
L and R resume their configured roles after wake. Releasing M early cancels
panel preparation. A held L or R still allows M to wake the display.

While the screen is off, EXT1 watches M and the next game deadline sets the
timer wake. L polling stops. After wake, the adapter clears L's retained event
and waits for all buttons to be released. A successful PMIC read is required
to establish that L has been released.

When the game or Settings is visible, the front buttons can wake the CPU and
required button polls supply a 5 ms deadline. Walking and interactive game work
retain their native timing instead of accelerating to consume queued input.

## GPIO restoration

EXT1's incremental API appends pins. Clear its old mask before switching
between M and M/R. After every sleep attempt, return both pads to digital GPIO,
including when the sleep controller rejects entry. Configuration failures
keep the CPU awake until a scheduled retry.

## Checks

Production-firmware simulation covers timeout, canceled and qualified M holds,
held-input suppression, RTC timer/EXT1 wake and digital GPIO restoration. The
[device test records](../../stick/docs/history/power-and-wake.md) record the
button, panel and boot trials. USB keeps the CPU awake during bench commands;
test physical sleep and wake on battery.

The [ESP-IDF sleep API](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/sleep_modes.html)
describes the wake-source and GPIO functions.
