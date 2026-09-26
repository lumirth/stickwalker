# Wake transition and current runtime estimate — 2026-09-26

## Wake finding and fix

The user's report was that button presses did not seem to wake the display.
The live production application was ready, its RTC seconds advanced, and both
native and physical display states were off. That observation rules out a
complete application hang at the time inspected; it does not establish what
physical button events preceded the snapshot.

A cold-wake probe using bench-generated holds found an intermittent delay:
Center could make the native game interactive while the physical panel was
still restoring at the observation point. Repeating from a new boot showed
that it eventually wakes, rather than remaining permanently asleep.

A queued gesture can already require the 62.5 ms scan cadence before the next
input poll. The runtime formerly shortened the pending deadline only if the
period changed during that poll. If the new period was already in effect,
the inactive one-second deadline survived. A gesture could therefore wait
almost one extra second before its first native scan. The physical event path
usually shortened it earlier, so not every input or phase reproduced this.

The runtime now caps the next sample deadline to the current fast period while
a wake gesture is pending, regardless of when it became pending. It preserves
the native hold/scan rule, game cadence, estimator, paired state and input layout.
The direct M hold remains the 500 ms convenience wake; short ordinary taps are
not a replacement for the wake hold.

Evidence, all retained under ignored `stick/.build/wake-diagnostic-2026-09-26/`:

- `cold-wake-initial.json`: held M restored the panel; Center had not completed
  physical restoration at the chosen observation point.
- `cold-wake-repeat.json`: three subsequent cold holds passed; the initial result
  was a delay, not proof of permanent panel failure.
- `latency-before-fix.json`: the narrower probe found the Center panel and supply
  still off at approximately 923 ms after injection.
- `queued-wake-before.log`: host regression failed on the stale deadline.
- `queued-wake-after.json`: all 16 lifecycle cases pass; the queued-wake case
  now executes 19 main ticks in 1.2 s.
- `latency-after-fix.json`: nine cold cycles on one boot, held M / Center / settings
  event repeated three times. Panel awake, PWM ready and LCD supply on at each
  roughly 0.9 s observation; settings opened and closed correctly.

The hardware probe uses software-generated inputs. It exercises both the polling
and native scan paths, shared supply, actual PWM and panel driver, but does not
verify actual switch actuation or visual appearance. No screen confirmation was
requested. The fix addresses a demonstrated scheduling delay; attribution of
all details of the user's physical-button symptom remains limited by that fact.

## Runtime prediction for the installed lifecycle design

Best planning estimate: approximately **four days**, with **three to five days**
as a practical everyday expectation. These are engineering estimates, not a
measured discharge result. A broad uncertainty range is about two to seven days,
depending on retained-domain current, brightness, movement and interaction.
The agreed one-week floor, two-week minimum useful result and month goal have
not been demonstrated and are not assumed in this prediction.

Use the nominal [250 mAh capacity](https://docs.m5stack.com/en/core/StickS3), with
200–225 mAh chosen as planning headroom for usable capacity, aging and cutoff.
The following whole-device battery currents are model assumptions informed by
the installed schedules and completed shutdown checks; they are not datasheet
values or measured current readings:

| Mode | Chosen current | Daily duration | Daily charge |
| --- | ---: | ---: | ---: |
| Stationary carry, screen off | 1.5 mA | 17.5 h | 26.25 mAh |
| Moving carry, screen off | 3 mA | 6 h | 18 mAh |
| Lit interaction, including screen tails and brief cues | 25 mA | 0.5 h | 12.5 mAh |
| Total | | 24 h | 56.75 mAh |

200–225 / 56.75 gives **3.5–4.0 days**; using the entire nominal capacity gives
4.4 days. Occasional IR adds some energy. Longer gameplay, higher brightness
and more sound push toward the lower end. Mostly stationary use with few checks
could plausibly approach five to eight days, but the background current is the
largest unresolved numeric input. This estimate excludes persistent faults,
stuck inputs or repeated sleep failures.

Remaining costs include frequent wake/entry work, retained RAM/RTC domains,
normal-mode BMI270 sensing and TFT/backlight energy. The manufacturer's much
lower L1/L2 reference states are not this retained-state firmware's moving-carry
current. Achieving weeks needs the remaining motion/wake/deeper-sleep work;
removing the former idle LCD/codec loads alone does not establish weeks.

## Frozen production installation

The production and diagnostic builds completed successfully. The production
application is frozen as wake-production-app.bin with its exact ELF and manifest
in the ignored evidence directory. App SHA-256:

    7778124e9eb41f2a59a766a1360f1a6e1575055340c98b6bcdcd2e39aae2f7a7

All three complete optical gate/producer function bodies and addresses match
the frozen baseline exactly. The entire storage partition before/after the
wake diagnostic block is identical to the independently reconstructed paired
save's storage snapshot. The final app-only flash and storage digest are checked
before reboot. No native src/ or include/ change, save import or erase occurred.

## Physical-switch follow-up: RTC ownership

The user reported that a real held M still failed after the deadline fix.
The generated-input tests had not qualified actual switch acquisition. A live
snapshot was not sufficient to attribute the failure to startup or execution:
subsequent USB-reset execution advanced normally. Retain that observation as
unresolved rather than claiming a startup hang was the cause.

The sleep adapter had a separate concrete defect: EXT1 preparation changes
GPIO11/12 into RTC IO, but every subsequent poll still used `digitalRead()`
without returning those pads to digital GPIO. The ESP-IDF 5.5
[EXT1 warning](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/sleep_modes.html#external-wakeup-ext1)
requires `rtc_gpio_deinit()` before digital use. The prior host sleep stub did
not model the ownership change, and generated input bypassed physical reads.

The adapter now releases RTC holds, deinitializes RTC muxing, and restores
input direction and pull-ups for both M/R pads at setup and after every
`esp_light_sleep_start()` return, including cancelled entries. Each failed
restoration is recorded and invokes the existing bounded setup retry. Native
game code, wake holds, GPIO5 and the receiver remain unchanged.

Focused verification (ignored wake evidence directory):

- `rtc-handoff-before.log`: regression is red when the stub models EXT1 owning
  both pads at sleep entry and checks their ownership at the next input poll.
- `rtc-handoff-after.json`: all 16 lifecycle cases pass, including successful,
  rejected, short and unexpected-error sleep returns.
- `rtc-mux-hardware.json`: bench command `j` intentionally selects RTC muxing on
  both physical pads without sleeping USB, then calls the real setup path.
  Both repetitions report `before=3 restored=1 after=0` with successful API
  results. This directly checks the hardware ownership transition.
- `rtc-mux-wake-latency.json`: all nine generated-input cold wake cycles pass.
- `physical-button-capture*.json`: no M edge was captured in the two timed
  observation windows. These records do not qualify physical switch actuation
  or demonstrate that the user's complete failure has ceased.

The new production app SHA-256 is
`12fa6823643b6d62d60242b2cb8fbc04deb877d41c0c274593c2348e8ed2f98b`.
The three full optical gate function bodies and addresses still match the
frozen baseline. The pre-follow-up storage snapshot exactly matches the
previous independently verified paired save. Application-only flashing and
full storage digest verification preserve it; no save import or erase is used.
This small GPIO handoff fix does not materially change the runtime estimate.

### Final production boot handoff

`rtc-mux-production-liveness.log` did not provide a valid running-app check:
its descriptor read was `0xbad00bad`, its purported ready byte was invalid,
and its clock field did not advance. Do not interpret those RAM reads as valid
application fields or assert a specific firmware crash cause from them.
Opening the existing Link with DTR/RTS set false before open produced a fresh
USB-UART reset boot. After closing that connection,
`rtc-mux-production-after-link.log` shows the correct descriptor
`0xabcd5432`, ready=1, and RTC seconds advancing across 1.5 seconds. No further
reset or flash followed that verified running state. The handoff sequence,
rather than `esptool run` success alone, is now the evidence for the installed
production application's liveness. Its underlying first-start behavior still
needs a controlled reset/control-line study if it recurs.
