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
