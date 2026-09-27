# M-only dark wake, immediate motion processing

## Installed behavior

- Physical M begins the existing 500 ms dark-screen wake in every layout and
  orientation. A held R or L cannot block that gesture.
- Dark input reads only M. L/R cause no menu action, sampling acceleration,
  panel preparation or PMIC button-status polling. The first successful PMIC
  read after wake discards an old L latch. The wake gesture is consumed until
  all keys have been released; a failed PMIC read cannot count as release.
- Dark sleep selects only GPIO11 and has no 100 ms side-button cap. Actual
  deadlines still include native activity/motion work and clock maintenance.
  Visible game/Settings selects GPIO11/12 and retains the bounded L check.
- Changing EXT1 masks explicitly clears the previous mask, because IDF's
  incremental enable API appends pins. Both RTC pads are restored after entry,
  including rejected opportunities. Configuration failures veto and retry.
- The native inactive/activity 1 Hz and moving/interactive 16 Hz schedules,
  FFT, motion estimator and sound/IR ownership gaps remain unchanged. No
  deferred sensor FIFO batches were implemented; that proposal is declined.

## Verification and retained attempts

Host checks passed:

| Check | Cases | Relevant coverage |
| --- | ---: | --- |
| Real input adapter | 8 | All four layouts in both orientations; ignored dark L/R; M wake with held L/R; stale L latch; transient failed PMIC reads; post-wake R navigation; Settings controls while native display is blank |
| Runtime lifecycle/fault trace | 16 | M-only mask and up to 999,000 us inactive sleep timer; moving native cadence; visible cap; mask changes both ways; RTC mux restoration; bounded failure recovery; display/audio lifecycle |
| Abandoned wake | 15 | Cancelled M preparation, shared sound ownership, failed supply acquisition, next accepted hold |
| Codec startup | 3 | Existing stage order and fault recovery |
| Native sound scores | 176 | Existing cold/warm timing and redraw stalls |

Both new policy regressions failed on the previous code: L/R activity kept
wake scans active, and dark sleep still selected both buttons and capped the
timer. They passed after implementation.

The connected Stick passed eight USB-awake generated-input checks: two abandoned
M taps, two accepted M holds, two ignored dark L events, and two Settings opens
following M wake (with clean closes). Accepted wakes returned LCD status `9c`,
pixel format `05`, PWM ready and PMIC output `04`; rejected gestures left the
panel asleep and output `00`. These are generated inputs, not physical-switch
sleep-wake qualification. USB continues to veto processor sleep.

The first hardware probe retained a legacy native Center injection and failed
its old 0.9-second expectation. That injection no longer raises the dark-screen
scan cadence under the chosen M-only policy. The probe was updated to exercise
M wake and Settings after M; the original output remains in
`hardware-wake-attempt1.json` and `.log`. No failed evidence was overwritten.

Reproduce host checks from the repository root:

```sh
python3 stick/audits/m_only_wake_trace.py
python3 stick/audits/idle_power_trace.py
python3 stick/audits/abandoned_wake_trace.py
python3 stick/audits/codec_startup_trace.py --output /tmp/codec.json
python3 stick/audits/sound_score_trace.py --output /tmp/sound.json
```

Build diagnostic and production sequentially with `stick/build_port.py`, using
`--bench-control` only for the diagnostic build. Hardware probe:

```sh
../bench/.venv/bin/python stick/audits/wake_latency_probe.py \
  --port /dev/cu.usbmodem1101 --output /tmp/wake.json \
  --repeats 2 --lcd-readback --abandoned-taps 2
```

## Installation and save preservation

Evidence is retained under ignored `stick/.build/m-only-wake-2026-09-27/`.
Production app SHA-256:
`5e29d017a468958a07f74da98e4d60602f1e8a28530a742ee5f1bd81631b6c43`.
The frozen receiver's three protected IRAM functions retain their addresses,
lengths and exact machine bytes. Production excludes bench command handlers.

The flash at `0x10000` matches the frozen production app. The complete storage
partition at `0x670000` matches its independently checked pre-production backup.
Both snapshot CRCs were checked, highest generation selected and journal replay
supported; the logical 64 KiB save matched the initial backup exactly after bench
checks (generation 569, no journal). Paired save SHA-256:
`671d392987cf62c9dd845c62e4c2ae13cc679dc828f788c5af44e047701120d5`.
CPU0-only production readback found ready=1 and advancing native RTC seconds;
the target was resumed. Private storage remains excluded from Git.

## Power interpretation and PMIC input follow-up

Removing ten-per-second stationary side-key wake opportunities reduces scheduled
CPU work; it does not measure energy per wake or total retained-domain leakage.
Moving carry still processes at 16 Hz. No new battery-life number is claimed.

M5Stack documents the PMIC IRQ output on ESP32 GPIO13, and its driver provides
separate button, GPIO and system masks. An interrupt-assisted visible L input
could avoid timer wake opportunities, then read/acknowledge the event over I2C.
The PMIC's classified click events and minimum 125 ms configurable click delay
must be distinguished from the raw pressed level and read-cleared latch at
`0x48`. Button latency, release/hold handling, unrelated-event masking and the
previous IRQ-mode LED behavior need verification before replacing this path.
The current installation does not enable GPIO13 wake.

[M5Stack IRQ wiring](https://docs.m5stack.com/en/arduino/m5sticks3/m5pm1),
[M5PM1 register definitions](https://github.com/m5stack/M5PM1/blob/main/src/M5PM1.h),
[policy decision](../../docs/adr/0002-m-only-dark-wake.md).
