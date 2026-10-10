# Use M to wake the screen

Holding physical M for 500 ms wakes the Stick in every layout and rotation.
L and R resume their configured roles after wake. Releasing M early cancels
panel preparation. A held L or R still allows M to wake the display.

While the screen is off, EXT1 watches M and the next game deadline sets the
timer wake. L polling stops. After wake, the adapter clears L's retained event
and waits for all buttons to be released. A successful PMIC read is required
to establish that L has been released.

When the game or Settings is visible, the front buttons can wake the CPU and
sleep is capped at 100 ms for L polling. Awake polls run every 5 ms. Walking
and interactive game work normally supply a 62.5 ms deadline.

## GPIO restoration

EXT1's incremental API appends pins. Clear its old mask before switching
between M and M/R. After every sleep attempt, return both pads to digital GPIO,
including when the sleep controller rejects entry. Configuration failures
keep the CPU awake until a scheduled retry.

## Checks

Host tests cover the adapter branches and sleep API calls. The
[device test records](../../stick/docs/history/power-and-wake.md) record the
button, panel and boot trials. USB keeps the CPU awake during bench commands;
test physical sleep and wake on battery.

The [ESP-IDF sleep API](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/sleep_modes.html)
describes the wake-source and GPIO functions.
