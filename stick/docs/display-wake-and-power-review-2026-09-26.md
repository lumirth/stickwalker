# Display wake correction and power review — 2026-09-26

## Confirmed display failure

The user heard sound after a held M wake but saw no picture. The matching native
application can advance and play sound while the physical panel remains off.
That report distinguishes game wake from LCD output and led to the delayed
ST7789 display-on command path.

The installed M5GFX implementation is decisive:

- `LGFXBase.hpp::writeCommand()` forwards to `Panel_Device::writeCommand()`.
- `Panel_Device::writeCommand()` writes to the bus without selecting the panel.
- `Panel_LCD::begin_transaction()` asserts CS; `end_transaction()` waits for
  transfer completion before releasing CS.
- `Panel_LCD::setSleep()` owns a transaction, while the port's delayed
  `IDMOFF`/`DISPON` writes had not done so.

The adapter now wraps those two writes in `startWrite()` / `endWrite()` after
its existing sleep-out deadline. Both supply-restored and rail-retained wake
paths use this completion step. Native screen availability and game cadence
are unchanged.

The actual module uses ST7789P3, as documented by
[M5Stack](https://docs.m5stack.com/en/accessory/display/Display_1.14_For_StickS3).
Its [datasheet](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1278/ST7789P3_SPEC_V1.0.pdf)
requires selected chip select for serial commands (pp. 48–49), specifies a
minimum 150 ns read cycle (pp. 37–38), and sends the 8-bit status immediately
following a four-wire command (p. 52). The generic driver's default dummy bit
shifted the initial readback; 16 MHz was also outside the read timing limit.
The board read setup now uses no dummy bit and 4 MHz. Write transfers still
use 40 MHz. This read setup is verified by the actual controller replies;
it is not an inference from a different ST7789 generation.

## Regression and hardware evidence

Artifacts are under ignored `stick/.build/wake-diagnostic-2026-09-26/`.

The host panel stub now models command selection and actual output-enabled
state. The cold-register initialization resets output-enabled, and an
unselected display-on command is ignored. `display-cs-before.log` fails with
`visible wake must send selected DISPON before presenting`. All 16 lifecycle
cases pass after the fix, including cold wake, warm wake while sound retains
the shared rail, image restoration and audio shutdown faults.

The bench-only `F` command performs a reversible hardware comparison without
sleeping USB or changing the game/save. It selects display-off, attempts the
former bare display-on write, and finally sends display-on inside a proper
transaction. Three actual-controller repetitions all return:

| Read point | RDDPM | Display-on bit |
| --- | --- | --- |
| After selected display-off | `98` | 0 |
| After unselected display-on | `98` | 0 |
| After selected display-on | `9c` | 1 |

`display-hardware-cs-proof.json` retains all replies. The ordinary startup
readback is power=`9c`, MADCTL=`b8`, pixel format=`05`, consistent with the
configured orientation and 16-bit pixels. The RDDPM bit definitions are in
the ST7789P3 datasheet p. 132. This directly establishes that the formerly
unselected command was ignored and that the corrected command reaches the
controller; it does not depend on visual feedback.

`wake_latency_probe.py --lcd-readback` additionally requires sleep-out and
display-on, idle mode off, and the configured pixel format after each cold
wake. Diagnostic commands and register reads are excluded from production.

## Assessment of the supplied battery review

The review is useful about the remaining design costs. It does not establish
new measured mode currents or a measured whole-battery lifetime.

| Point | Current evidence and implementation consequence |
| --- | --- |
| 100 ms L polling limits stationary sleep | Confirmed in `StickSleepUntil()`. Removing it requires a validated PM1 interrupt with correct masking/acknowledgement and working onset/hold/release semantics. Keep the fallback until that works. |
| 16 Hz screen-off movement incurs repeated host work | Confirmed in native motion mode. Short FIFO batches are a suitable next design, preserving the native estimator and chronological event ordering. |
| Held buttons prevent explicit sleep | Confirmed. A future sleep policy must account for already-held level-sensitive wake pins, ongoing chord windows and release detection; dropping the veto alone would create immediate wake/rejection loops. |
| Preserve lit screen availability | Agreed. Keep native availability and quarters-of-a-second rendering; the TFT's panel scan is a separate cost. No earlier blanking/dimming or reduced native game cadence is justified by this review. |
| Lower-power accelerometer mode | Reasonable after host wake overhead work and motion validation. Keep the current filter/rate first; Bosch step-counter substitution does not preserve the native estimator. |
| Audio shutdown failure may strand an owner | The current service already retries `stop_output()` on `shutdown_retry_us` even after `speaker_ready` is false. The injected codec fault regression verifies release; this is not a newly demonstrated persistent leak. |
| Retained PSRAM/light-sleep floor and deeper resume | Plausible constraints requiring battery-terminal current evidence. Any deeper resume needs continuous clock, game state and eligible motion history without a visible reboot. |

For FIFO work, replay must choose chronological acceleration from the current
sensor mode onto the native 62.5 ms sample timeline; simply feeding every
100 Hz FIFO frame to MainTick would change the estimator. Drain eligible history
before delivering a foreground button event. Preserve the native sampling gaps
while sound or IR owns shared state. Flush/discard those excluded intervals
rather than crediting their motion afterward.

The sensible optimization order remains PM1 interrupt, held-gesture sleep,
short screen-off FIFO batching, then sensor/deeper-sleep selection informed by
current and motion evidence. None of those paths is declared implemented by
this display milestone. In particular, the claim that ten wakes per second
cannot average a particular current is a budget argument, not a board-current
measurement. The current four-day planning estimate is not revised upward by
fixing the display command transaction.

## Production installation and preservation

All nine cold-wake cases passed with controller output enabled: three M holds,
three Center holds and three settings events. Every post-wake read returned
power=`9c`, MADCTL=`b8`, pixel format=`05`. These generated inputs qualify the
panel command path; they do not qualify physical switch actuation or visual
appearance. RTC-pad restoration for physical M/R was committed separately as
`f79e04a` and remains in this build.

The frozen production app was flashed only at `0x10000`; its write digest
matched. Production app SHA-256:

    a782fd21b5aa5380481b4412406948faabbba09808b0ef44137c6bb859cf3024

`display-proof-production-manifest.json` verifies that all three electrical
sampler function bodies and their addresses match the frozen baseline. No new
optical reliability claim follows from these display checks.

The initial whole-filesystem comparison failed and is retained as
`display-production-storage-verify.log`. Independent CRC-valid slot/journal
reconstruction then showed the entire active course-resource range
`0x8f00..0xb7ff`, both native status records and both device-ID records exactly
match the pre-diagnostic backup. Registered/has-Pokémon flags remain set.
The save mirrors remain equal and checksum-valid.

The logical EEPROM is not entirely identical: its stored clock advanced
23 seconds, diary cursor advanced 21→22, and entry 21 contains action `0x15`.
The native `SocialApply(SOCIAL_EVENT_BORED)` appends exactly that action
(event ID 5 + base 16); a diagnostic Center input accepted the offered event.
All changed bytes are confined to that one 136-byte diary entry and the two
24-byte save payloads/checksums. Steps, watts and Pokémon-minute counters are
unchanged. The paired Pokémon and its artwork were not replaced, and the
updated native diary/clock state was retained. The fresh full filesystem
snapshot subsequently verified against device flash with a matching digest.
`display-save-comparison.json` records both the failed whole-save equality and
successful paired-data checks. Private data remain excluded from Git.

After production startup and USB control-line release, the supported CPU0-only
JTAG snapshot found the application descriptor `0xabcd5432`, adapter ready=1,
and native RTC advancing `0x32495723`→`0x32495724` across a 1.5-second resume.
The debugger resumed the chip on exit; no further reset or flash followed.
`production-serial-handoff.json` and `production-boot-live.log` retain this
handoff. Bench commands are absent from the installed production image.
