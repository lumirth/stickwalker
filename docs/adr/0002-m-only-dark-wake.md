# M-only wake while the game screen is off

The user chose physical M as the sole button that begins a dark-screen wake,
independently of layout and orientation. Hold M for the existing 500 ms gesture;
L and R resume their configured roles after wake and release. A held R or L
must not prevent M from waking. An abandoned M press cancels panel preparation.

Screen-off operation does not read the PMIC button register, add R to EXT1,
cap sleep to 100 ms, or accelerate native sampling for L/R activity. Discard a
retained L event on return to visible input and consume the wake gesture until
all switches are released. Failed PMIC reads do not count as a quiet release.

Visible game and Stick Settings controls retain front-button wake and the
100 ms maximum sleep for the PMIC's latched L input. Awake input polls run at
5 ms; moving/interactive native work usually supplies a 62.5 ms wake deadline.
The visible cap keeps L usable without adding an unverified PMIC button-interrupt path.
An open Settings overlay counts as visible even if the native game is blank.

EXT1's incremental enable API appends pins. Explicitly disable the old mask
before changing between M and M/R. Restore both RTC-capable pads to digital
GPIO after every sleep return, including rejected entries. Configuration errors
veto sleep and retry at a bounded rate.

Motion processing remains immediate at the existing 1 Hz inactive/activity
and 16 Hz moving/interactive schedules, with native audio/IR ownership gaps.
No sensor mode, ODR, FFT, estimator, protocol, or EEPROM format change is part
of this decision. Deferred FIFO motion batching is declined.

Host validation covers actual adapter branches and simulated electrical API
calls. USB bench probes exercise generated gestures, rail/panel readback, and
production boot. Neither proves physical-switch wake or whole-device current;
USB continues to veto Light-sleep. No improved lifetime is claimed from timer
counts alone.

[ESP-IDF sleep API](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/sleep_modes.html)
