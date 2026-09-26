# Power lifecycle hardware validation — 2026-09-26

Status: hardware lifecycle checks and focused host regressions completed;
final production installation and preservation verification recorded below.
This is not a battery-life qualification or a new optical reliability campaign.

## Supported debugging procedure

Use the [manufacturer-source debugging report](debugging-practices-2026-09-26.md)
for USB, JTAG, crash-dump, PMIC and current-measurement procedures. Awake USB
checks cannot qualify battery sleep. Bench `Y` now arms a finite five-second
trial that starts only after a successful PMIC observation of absent VBUS.
There is no USB-sleep override. The probe's `battery-arm` outcome is explicitly
incomplete; `battery-read` requires a completed retained trial after reconnect.
Existing unchecksummed RTC diagnostics are useful for the observed CPU resets,
not proof of retention across brownout or power loss. Versioned/checksummed
records would be needed for stronger crash-retention claims.

Production and bench now preserve the success/failure of the PMIC VIN read.
An unknown reading vetoes sleep until a successful scheduled read. M5Unified's
convenience `getVBUSVoltage()` returns zero on read failure; using it to authorize
sleep was a concrete bug. Polling is every second; insertion between polls is
not an instantaneous hardware USB interlock.

Sleep entry rejection (259) and too-short duration (258) return to normal work
without invalidating wake configuration or imposing a one-second awake period.
Unexpected failures retain bounded recovery. The previous visible trial's four
errors lacked retained codes; the new handling cannot retrospectively identify
them. Focused regression proves the distinction in the production scheduler.

## Evidence ledger

All raw logs, firmware/ELF pairs, save backups, crash data and trial JSON remain
in ignored `stick/.build/power-hardware-2026-09-26/`. Historical failures remain
there; the following rows distinguish useful evidence from incomplete attempts.
No historical USB sleep trial is treated as battery-current evidence.

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

## Paired data

Before any flash, a full 8 MiB image was backed up. Both native status/save
mirrors validated with the original checksum, the device remained registered,
and a paired Pokémon was present. Snapshot slots and six CRC-valid journal
records were independently reconstructed.

After the diagnostic block, all 65,536 logical EEPROM bytes matched the initial
reconstruction exactly:

    324c0e5d46de68c861195b71132179cc1fed7eea94c02918d6aa7717d9f11885

The fresh pre-final filesystem differs physically because boot checkpointed
the journal: snapshot generation 566, zero journal records. Independent replay
still gives the exact same logical EEPROM hash and zero changed addresses.
The coredump partition also matches the initial backup, so no new flash dump
was observed during this block.

Private save contents and binaries are excluded from Git. Final application-only
flash was verified against the fresh entire 1.5 MiB storage snapshot while
stopped in ROM: digest matched. The application write digest also matched.

Installed production app SHA-256:

    b94ddf991fb495b0ac01849f855bd4426e69632700b19fd01a9f998723465b68

Its complete measure_ir_gate, aligned_next and run_gate_segment bodies and
addresses are byte-identical to the frozen production baseline in the original
lifecycle report. Both diagnostic and production builds completed successfully;
the installed image and exact ELF/source hashes are frozen in
installed-production-manifest.json. The native src/ and include/ code was not
edited. Independent input layout and Light/Dark settings are included, with
visual verification left to the user as requested.

## Battery conclusions and remaining measurements

Verified changes remove idle I2S/codec and shared peripheral supply, retain
background sensor access, reduce the non-IR CPU clock, permit processor sleep
between original scheduling deadlines, and suppress redundant LCD transfers.
Unused gyro/temperature, green LED and idle IR boost are off. These are real
lifecycle improvements; they do not establish a numeric whole-device current.

The remaining costs include the 100 ms power-key timer wake cap, RTC peripheral
retention, retained RAM, BMI270 normal-mode 100 Hz sampling/filtering, movement's
16 Hz application work, backlight during native visible periods, and actual game,
audio, persistence and IR workload. Sensor FIFO/lower-power modes and deeper
checkpoint sleep remain separate implementation decisions requiring accuracy
and wake evidence. No one-week, two-week or month lifetime has been established.

The board has no documented battery-current/coulomb counter. A USB input meter
includes charging and the wrong power path. A useful current test requires a
low-burden instrument in series with the actual battery path, including all board
loads; an ammeter connected to the parallel BAT header is insufficient. That
instrument/fixture is not present. USB voltage and sleep counters cannot replace
this measurement. The source-faithful 3DS peer at 10.1.10.139:5080 also timed out
in this block, so a new live transaction could not be qualified.
