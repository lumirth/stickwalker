# Power and wake trials, September 26-27, 2026

This record preserves measured outcomes and failed observations from successive
images. The [current power guide](../power.md) defines the implemented policy.
The early trials used an M/R wake mask and a dark-screen L polling cap; the later
M-only policy superseded both. No trial measured whole-device current or runtime.

## Initial lifecycle ledger

Raw artifacts are local under `stick/.build/power-hardware-2026-09-26/`.

| Artifact / attempt | Outcome and meaning |
| --- | --- |
| `sleep.json`, `sleep-reconnect.json` | USB-connected sleep commands lost subsequent observation. No captured successful sleep count; incomplete, not passes. |
| `startup.json`, startup/boot diagnostic logs | Awake startup/configuration observations; stage reporting introduced after PMIC startup failures. |
| `dark-focused.json` → `dark-recovered.json` | First retained dark observation: 56 returns, 4.956 s in sleep calls. Recovered record exists; USB output itself was not sufficient. |
| `dark-final.json` | Harness could not retrieve its expected previous record; failed observation. |
| `dark-bus-diagnostic.json` | PMIC startup failure after warm reset; SDA/SCL both HIGH afterward do not identify the I2C failure mechanism. |
| `dark-recovery.json` | Completed approximately 5.093 s dark trial: 56 successful sleep returns, five accelerometer reads, native clock +5 s, zero recorded failures. |
| `lit-recovery.json` | Completed sampling trial but four sleep entry errors and only 17 successful returns / 0.912 s in sleep calls. Failed power-scheduling result; original error codes unavailable. |
| `lit-reject-cause.json` | Completed approximately 5.025 s visible trial: 82 returns, 80 accelerometer reads, clock +5 s, zero recorded failures. |
| `lit-reject-cause-2.json` | USB device disappeared; ROM recovery could not open the missing port. Failed observation, required physical replug. |
| `audio-reconnect.json` | Incomplete initial audio observation; later `audio-final.json` is the completed check. |
| `audio-final.json` | Two cold-supply playback cycles. LCD/audio supply and amp 00→0c→00, codec/I2S stopped after hold, zero begin/power errors. No new audible-fidelity claim. |
| `rx-final.json` | Two-second receiver worker start/stop, 240 MHz startup prerequisite, return to 80 MHz and IR boost off. PMIC first read failed and recovered on retry 1 in this actual run. No optical peer payload; not an exchange test. |
| `jtag-availability.log` | Espressif OpenOCD examined both cores using built-in USB JTAG; no halt, breakpoint, reset or flash command in that probe. |
| `pre-flash-coredump-analysis.log` | Initial backup already contained a CRC-valid historical dump. Official decoder rejected a mismatched ELF; matching historical ELF not found. No cause or date attributed to today's tests. |
| `rejection-host-before-fix.log` / `rejection-host-after-fix.log` | Regression exposed the one-second veto; revised scheduler permits ordinary rejection without resetting wake configuration. |
| `vbus-regression-before.log` / `vbus-regression-after.json` | Persistent failed power reads incorrectly allowed sleep before fix. Fifteen host lifecycle cases now pass, including unknown VBUS veto and transient read recovery. |
| `final-host-ir-regression.log` | Three TX lifecycle/symbol tests pass; no receiver/payload qualification inferred. |
| `usb-veto-final.json` | Final diagnostic build kept sleep count/error count zero during six seconds after Y arm, with measured VBUS 5218 mV. |
| `production-jtag-*.log` | Initial running-memory read was rejected (target not halted). Subsequent dual-core reads were inconsistent with the ELF while CPU1 could not be examined. The installed supported ESP_ONLYCPU=1 configuration returned ready=1, USB veto=1 and the expected application descriptor magic. Brief halt/resume was followed by a fresh standalone reset; no optical measurement used these debugger sessions. |
| `flash-interim-production.log` | A previous production output was flashed while a diagnostic rebuild was still running. No tests qualified that transient image, no storage partition was flashed; subsequent images were frozen only after successful build completion. |

Successful sleep-call duration includes entry/wake overhead and is not an exact
asleep duty fraction. These finite trials verify scheduling and physical returns,
not long-running battery wake reliability, movement accuracy or energy savings.
PMIC bounded retry recovered an observed failure; its physical cause is unresolved.
## Panel and physical-input findings

The display review established that a command without chip-select ownership
was ignored. RDDPM remained `98` after an unselected display-on command and
changed to `9c` after the selected command. Following restoration, MADCTL was
`b8` and pixel format `05`. This verifies controller state, not perceived shades
or a battery-current reduction.

The wake diagnostic found a pending gesture could inherit the inactive
one-second deadline. Before correction, a generated Center gesture still had
the panel/supply off near 923 ms. After correction, all nine generated cold-wake
cycles had panel, PWM and supply ready near the 0.9-second observation.
A subsequent physical held-M failure showed that generated inputs had not
qualified switch acquisition.

EXT1 preparation left M/R in RTC IO while polling used digital reads. The
corrected handoff restores digital ownership after every sleep return. The
hardware mux probe reported `before=3 restored=1 after=0` twice. Two timed
physical-button windows captured no M edge; they did not demonstrate resolution
of the reported physical-switch failure.

`rtc-mux-production-liveness.log` had invalid descriptor `0xbad00bad` and no
clock progression, so its RAM values cannot identify a crash cause. A fresh
USB-UART reset using DTR/RTS false before opening the connection was followed
by valid descriptor `0xabcd5432`, ready=1 and RTC progression over 1.5 seconds.
That reset-capable recovery and its post-reset verification are both retained.
Wake artifacts are under `stick/.build/wake-diagnostic-2026-09-26/`.

## M-only policy verification

The September 27 host checks covered eight input configurations, sixteen
runtime/fault cases, fifteen abandoned-wake cases and three codec cases.
The dark-mask/cadence regressions failed on the preceding code and passed
after the M-only change.

Eight USB-awake generated-input trials passed: two abandoned M taps, two
accepted M holds, two ignored dark L events and two Settings opens after wake.
Accepted wakes reported LCD status `9c`, pixel format `05`, PWM ready and PMIC
output `04`; rejected gestures left output `00` and the panel asleep.
These observations do not qualify physical-switch sleep/wake on battery.

The first probe failed its obsolete native-Center injection expectation at
0.9 seconds. It was changed to exercise M wake; `hardware-wake-attempt1.json`
and its log remain under `stick/.build/m-only-wake-2026-09-27/`.

## Installation identities and storage checks

Each listed image had app-only flash verification and retained sampler machine
bytes. Storage checks apply to its own local backup, not to a future candidate.

| Milestone | Production app SHA-256 | Local evidence directory |
| --- | --- | --- |
| Lifecycle | `b94ddf991fb495b0ac01849f855bd4426e69632700b19fd01a9f998723465b68` | `power-hardware-2026-09-26/` |
| Wake deadline | `7778124e9eb41f2a59a766a1360f1a6e1575055340c98b6bcdcd2e39aae2f7a7` | `wake-diagnostic-2026-09-26/` |
| RTC GPIO handoff | `12fa6823643b6d62d60242b2cb8fbc04deb877d41c0c274593c2348e8ed2f98b` | `wake-diagnostic-2026-09-26/` |
| Panel restoration | `a782fd21b5aa5380481b4412406948faabbba09808b0ef44137c6bb859cf3024` | `wake-diagnostic-2026-09-26/` |
| Abandoned wake cleanup | `4fb5efcec62ace583f9b19ec46e35b0f677c2f39314f27a437725de8cde56398` | `abandoned-wake-2026-09-27/` |
| M-only policy | `5e29d017a468958a07f74da98e4d60602f1e8a28530a742ee5f1bd81631b6c43` | `m-only-wake-2026-09-27/` |

The lifecycle trial's independently reconstructed 65,536-byte EEPROM hash
remained `324c0e5d46de68c861195b71132179cc1fed7eea94c02918d6aa7717d9f11885`.
Boot checkpointed six valid journal records into snapshot generation 566;
physical filesystem bytes changed but logical EEPROM did not.

The panel-restoration block retained an initial failed whole-filesystem equality
check in `display-production-storage-verify.log`. Independent reconstruction
found course resources, status/device IDs, paired Pokémon and save checksums
preserved. Logical EEPROM did change: the clock advanced 23 seconds, diary
cursor moved from 21 to 22, and entry 21 recorded native boredom action `0x15`
after a generated Center gesture accepted it. Changes were confined to that
136-byte diary entry and the two 24-byte save payloads/checksums. Steps, Watts
and Pokémon-minute counters were unchanged. The resulting filesystem then
matched a fresh snapshot. `display-save-comparison.json` preserves both results.

The later abandoned-wake and M-only trials preserved logical EEPROM hash
`671d392987cf62c9dd845c62e4c2ae13cc679dc828f788c5af44e047701120d5`.
Checkpointing moved generation 568 to 569 with zero changed logical addresses.
The M-only production flash also matched its complete pre-production storage
partition; CPU0 readback found ready=1 and advancing RTC after resume.

Raw saves stay private. Interrupted-write recovery, physical walking calibration,
long-running wake reliability and battery energy still need candidate qualification.
