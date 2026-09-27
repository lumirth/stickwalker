# Battery review: resolved memory mode and abandoned wake

## Review assessment

The complete supplied review was read and checked against the current port.
The established shutdown paths remain implemented. The review's follow-up
PSRAM correction is incorrect for the installed image:

- The board file has a base `build.psram_type=qspi` value, but its first PSRAM
  menu option overrides that value to `opi`.
- Arduino's expanded properties for the actual target give `build.psram_type=opi`
  and `build.memory_type=qio_opi`.
- The frozen production ELF's line table identifies `esp_psram_impl_octal.c`.
- M5Stack specifies 8 MB octal PSRAM for this model. [StickS3 specification](https://docs.m5stack.com/en/core/StickS3).

The build script now spells out `PSRAM=opi` so this does not depend on the menu
default. It selects the same mode as the installed baseline, not a new memory
configuration. Neither mode selection nor a 48 KB allocation establishes the
actual battery leakage of retained memory. That requires current evidence.

| Claim | Assessment |
| --- | --- |
| Screen-off stationary sleep is capped at 100 ms for L polling | Confirmed. The RTC domain is retained and timer returns cause an input poll. |
| Moving carry schedules native motion work at 16 Hz | Confirmed. The inactive period is one second; moving period is 62.5 ms. FIFO batching is still unimplemented. |
| A failed wake gesture can incur a complete panel wake and 750 ms tail | Confirmed before this fix; reproduced through actual input and panel adapters. |
| Stick Settings has its own 90-second visible period | Confirmed. This is separate from the native game timeout and adds interactive energy. |
| LCD can sleep while sound retains the common supply | Confirmed. LCD sleep, amplifier hold and supply-off are different boundaries. |
| Failed USB voltage reads veto sleep | Confirmed conservative policy; no evidence here of persistent battery-only read failure. |
| Failed codec suspension can retain the sound owner | Temporarily true, but the existing bounded retry continues after I2S stops. The lifecycle fault regression covers eventual release after a transient failure. |
| A held key keeps the foreground polling path awake | Confirmed. Supporting sleep during held gestures requires release/hold handling, not simply removing the veto. |
| A failed IR worker stop can retain 240 MHz | Confirmed safety behavior while a worker may still be running; no new occurrence demonstrated. |
| Flash is written only on the hour | Too broad. Hourly saves are one source; settings, diary events and protocol/storage updates also write. It is not written for every step. |
| A week is possible only after reducing wake frequency; a month cannot survive the lit window | Plausible budget concerns, not established hardware bounds. Wake count alone does not give wake energy; actual interactive current and usable capacity are also unknown. |

The review's FIFO ordering, native sampling-gap policy and staged sensor-mode
validation are consistent with the existing motion ADR. Simply feeding all
100 Hz FIFO records into a 16 Hz native estimator would change its temporal
meaning. A real batching implementation must select chronological observations
on the native timeline, preserve clock/event ordering, flush before input, and
exclude sound/IR-owned intervals. The hardware counter is not a replacement
for the settled native estimator.

## Implemented consequence

A dark-screen gesture now cancels panel preparation when all physical keys are
released, unless that poll captured a side-key action. A visible game or Stick
Settings screen is protected by the panel's backlight request.

- During reset preparation, release cancels immediately. A separate reset flag
  remembers lost controller state even if audio keeps the shared rail alive.
- After Sleep-out has started, cancellation completes its existing 130 ms
  wait, skips Display-on, then sends Sleep-in and releases display ownership.
- If preparation already finished while a key was held, release sleeps the
  panel immediately instead of waiting out the 750 ms lease.
- A failed supply acquisition drops its ownership claim immediately, so a
  later PMIC retry cannot power the rail after an abandoned gesture.
- Valid wake holds retain the existing gesture thresholds and asynchronous
  preparation. The next valid hold after cancellation still works.

This removes the avoidable tail; it does not eliminate the initial supply/reset
energy of every bump. The native game, screen availability, audio startup/hold,
motion schedule and optical receiver are unchanged.

## Focused validation

`python3 stick/audits/abandoned_wake_trace.py` compiles the unchanged production
input, controls, panel, display-memory, shared-power and backlight adapters
against hardware/native callback stubs. The pre-fix code failed after a 5 ms
press because the display still owned its rail after release. The final code
passes 15 cases: holds of 5, 25, 100, 180, 250, 450 and 550 ms, each with and
without a sound owner retaining the supply. Every cancelled case additionally
requires a successful subsequent 560 ms hold. No new Display-on command is
allowed after cancellation; ongoing Sleep-out must settle safely.
The fifteenth case injects failed supply-on throughout a 10 ms tap, then allows
the PMIC to recover. It reproduced an ownership leak before the additional
cleanup: the delayed power service enabled the rail after release. The final
adapter drops the claim and keeps the rail off, then accepts the next hold.

The existing 16 lifecycle/fault cases also pass. These are control-flow and
ownership checks, not physical switch, panel appearance or current tests.
Bench command `V` supplies one software-generated 40 ms M tap for an awake-USB
hardware check; it exists only in diagnostic builds. Current/energy measurement
is not inferred from successful command or rail-state readback.

On the connected board, three generated 40 ms taps passed: after each, the
panel was asleep, PWM unallocated and PMIC GPIO output `00` (shared supply and
amplifier off). Three subsequent cold wakes also passed: M hold, native Center
hold and settings event. Each returned panel status `9c`, pixel format `05`,
backlight ready and GPIO output `04`. These awake-USB checks used generated
inputs; they do not qualify physical switch actuation or battery sleep.

Evidence and final installation results are retained in ignored
`stick/.build/abandoned-wake-2026-09-27/`. Private paired data stays excluded
from Git.

Independent snapshot/journal replay gives identical pre/post logical EEPROM
SHA-256 `671d392987cf62c9dd845c62e4c2ae13cc679dc828f788c5af44e047701120d5`,
with zero changed addresses. Boot checkpointed three journal records from
generation 568 to 569; that physical storage change did not change the save.
The production image SHA-256 is
`4fb5efcec62ace583f9b19ec46e35b0f677c2f39314f27a437725de8cde56398`.
Both builds completed successfully. The complete `measure_ir_gate`,
`aligned_next` and `run_gate_segment` machine bodies and addresses are identical
to the frozen prior production image; bench input/acoustic commands are
excluded from production. This is not a new optical exchange qualification.
The intermediate production image (before failed-acquisition cleanup) was
flashed and verified while stopped in ROM; its binary, ELF and manifest remain
as `interim-production-*`. The final image supersedes it. The initial harness
compile failure, first failing regression and later fault regression are also
retained; none is counted as a passed check.
The final application and complete storage partition were verified against
their frozen images. CPU0 readiness and an advancing game clock confirmed
production startup; the processor was resumed after that debug check.

## Battery prediction and remaining work

The roughly four-day planning estimate remains unchanged. The cancellation
benefit depends on accidental-press frequency and is not quantified here.
One week, two weeks and a month remain targets, not measured results.

The substantial next work remains validated PM1 interrupt wake, held-gesture
sleep, then short screen-off motion batches. The common peripheral shutdown
and audio hold should not be reworked merely because the rail can outlive a
dark screen. Actual battery-terminal energy for stationary carry, moving carry
and interactive use is still missing; the board has no documented current
counter, and USB charging current does not substitute for battery-path current.
