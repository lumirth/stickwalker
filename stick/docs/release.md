# Stickwalker release qualification

The release candidate uses production firmware and original placeholder resident
artwork. Public release is pending qualification of its exact firmware bytes and
completion of runtime distribution materials. The project remains private until
publication is authorized.

## Software preparation

- The builder works from this repository, generates artwork without the H8
  compiler and pins M5Stack 3.3.9, M5Unified 0.2.21 and M5GFX 0.2.29.
- Host checks cover utility recovery, input layouts/chords, byte/bit layouts,
  EEPROM mirrors, diary/gift boundaries, RTC ticks, scratch pointers, compressed
  data, display commands, optical decoding and power/audio lifecycle faults.
- Packaging requires clean committed source, matching input/output hashes,
  production mode and placeholder artwork. It includes source, firmware,
  symbols, installation instructions and checksums; it excludes flash backups,
  received game data, local artwork and merged full-flash images.
- CI is configured for Linux and macOS host checks. A local macOS pass does not
  establish that the remote Linux job has run.

## Existing hardware evidence

The prior production application SHA-256 is
`5e29d017a468958a07f74da98e4d60602f1e8a28530a742ee5f1bd81631b6c43`.
Its saved 2026-09-27 manifest records installed-app verification, unchanged
filesystem bytes, preserved paired storage and an advancing RTC after resume.
That is historical evidence for that image, not verification of a new candidate.

Earlier integrated firmware completed a real HeartGold walk-start transfer.
Source-faithful 3DS trials completed repeated `back`/`put` operations with exact
course readback. Two longer diagnostic runs also retained optical/checksum
failures; the successful operations do not erase those failures. See
[validation history](validation-history.md) and
[M-only wake verification](m-only-wake-2026-09-27.md) for the evidence boundaries.

## Remaining release gates

Record the candidate commit and app SHA-256 with every result. Retain failed
attempts in the qualification ledger.

1. Back up a Stick's full flash. Install the exact candidate and independently
   verify the application bytes. Confirm prior registration, Pokémon, settings
   and valid EEPROM mirrors after an app-only update.
2. The packaged `ir-timing.json` must bind the candidate ELF hash to matching
   machine bytes for `measure_ir_gate`, `aligned_next` and `run_gate_segment`.
   Release preparation confirmed all three match the frozen production reference.
   This preserves known acquisition code; it does not replace live reception.
3. With USB disconnected, physically exercise M-only 500 ms wake in each
   layout/rotation, ignored dark L/R, abandoned short M taps, post-wake navigation,
   Settings, display restoration, short sounds and normal clock progression.
4. On one frozen production image, repeat real HeartGold/SoulSilver walk-start
   and return transfers. Record whole-operation successes/failures, distance,
   orientation, elapsed time, registration and independent EEPROM verification.
   Include at least one retail SoulSilver trial before claiming support verified
   on both games. Quantitative diagnostic trials qualify their own image only.
5. Validate native motion axis scaling and walking/stationary behavior on the
   physical board. Measure battery current with USB unplugged and a stated use
   profile before publishing runtime claims. The one-month goal is unmeasured.
6. Fault-inject interrupted EEPROM journal/image writes on hardware and verify
   recovery without formatting or losing the last completed save.
7. For public binaries, finish the Arduino/ESP-IDF component notices,
   corresponding dependency source and relinking materials. The bundled M5 MIT
   notices cover the two M5 libraries, not every linked runtime component.
8. Run the configured CI, check the final source/firmware archives and obtain
   publication authorization before changing repository visibility or publishing
   a GitHub release.

No board was connected during the release-preparation run. Packaging records
`hardware_qualified: false`; replace that only with a qualification record tied
to the exact candidate hashes and supported public claims.
