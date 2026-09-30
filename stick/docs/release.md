# Preparing a release

A release needs a reproducible software package and a qualification record for
its exact firmware bytes. Previous hardware results are summarized in
[validation history](validation-history.md); they do not qualify a new image.

## Prepare the artifacts

From a clean, committed checkout, run:

```sh
uv sync --locked
uv run python stick/check.py
uv run python stick/build_port.py --output-dir stick/.build/candidate
uv run python stick/check_timing.py --elf stick/.build/candidate/PwStick.ino.elf --tool-prefix /path/to/xtensa-esp32s3-elf- --output stick/.build/candidate/ir-timing.json
uv run python stick/package_release.py --build-dir stick/.build/candidate --output-dir stick/.build/release
```

The [build guide](build.md) covers Arduino setup and pinned versions. Find the
`xtensa-esp32s3-elf-` tools under the Arduino data directory at
`packages/m5stack/tools/esp-x32/2601/bin/`.
The timing check compares `measure_ir_gate`, `aligned_next` and `run_gate_segment`
with the frozen production machine code and binds its report to the candidate
ELF hash. Matching these bytes preserves the known acquisition implementation;
live reception still needs testing.

Packaging checks source and output hashes, production mode, placeholder artwork
and the absence of diagnostic markers. It produces a source archive, firmware
archive and `SHA256SUMS`. The firmware archive includes symbols, installation
instructions, manifests and M5 library notices. The packager records
`hardware_qualified: false`; qualification is a separate record tied to the
commit, app hash and ELF hash.

## Qualify the candidate

Keep a ledger with the candidate identities, setup, observations and failed
attempts. Distinguish physical-switch tests from generated gestures and retail
Game Card trials from diagnostic peers.

1. [Back up and install](install.md) the exact candidate. Read back and verify
   the app bytes independently. For an app-only update, confirm registration,
   Pokémon, settings and valid EEPROM mirrors survive.
2. Disconnect USB. Exercise the 500 ms M wake in every layout and rotation,
   ignored dark L/R, abandoned short M taps, navigation after release, Settings,
   display restoration, short sounds and clock progression.
3. On one frozen production image, repeat retail walk-start and return
   transfers. Record whole-operation successes and failures, game/version,
   distance, orientation, elapsed time and independent EEPROM verification.
   Include a retail SoulSilver trial before claiming verification on both games.
4. Check motion axis scaling and walking/stationary behavior on the board.
   Measure whole-device battery current with USB disconnected and a stated use
   profile before publishing runtime claims.
5. Interrupt EEPROM journal/image writes on hardware and verify recovery
   without formatting or losing the last completed save.

Use the [debugging guide](debugging.md) for retained diagnostics and the limits
of USB/JTAG observations.

## Prepare GitHub publication

Run the Linux and macOS CI jobs for the final commit. Check the unpacked source
and firmware archives, their installation links and checksums.

Before distributing binaries, complete the Arduino/ESP-IDF component notices,
corresponding dependency source and relinking materials described in
[third-party notices](../../docs/third-party.md). The bundled M5 MIT notices
cover the two M5 libraries; the runtime distribution materials are still incomplete.

Write release notes with the supported board, commit and app hash, qualification
results, known limitations and installation link. Attach the source archive,
firmware archive, checksums and qualification record. A candidate with an open
gate must be labeled as a prerelease and describe the missing evidence.
