# Preparing a release

A release contains installable firmware, source, runtime dependencies and a test
record tied to its exact firmware bytes. Use a prerelease for early testing;
reserve a stable release for an image that completes the device checks below.

## Build and check

From the intended checkout, run:

```sh
uv sync --locked
uv run python stick/check.py
uv run python stick/build_port.py --output-dir stick/.build/candidate --retain-build
uv run python stick/check_timing.py --elf stick/.build/candidate/PwStick.ino.elf --tool-prefix /path/to/xtensa-esp32s3-elf- --output stick/.build/candidate/ir-timing.json
```

The [build guide](build.md) covers Arduino setup and pinned versions. The
`--retain-build` option saves the objects and link command used by the candidate.
Find `xtensa-esp32s3-elf-` under the Arduino data directory at
`packages/m5stack/tools/esp-x32/2601/bin/`.

The timing check compares `measure_ir_gate`, `aligned_next` and `run_gate_segment`
with the frozen production machine code and binds its report to the candidate
ELF. Matching those bytes preserves the acquisition implementation used in the
recorded infrared trials.

## Assemble dependency materials

Use the source archives and `sources.json` from the previous release's runtime
kit, or obtain the pinned archives recorded in
[`runtime-sources.json`](../runtime-sources.json). Keep their hashes and original
notices. The ESP-IDF archive contains commit
`735507283d5b2f9fb363a1901172dbd9e847945d` and the recursive submodule revisions
recorded in that inventory, with Git metadata excluded.

Pass the installed SDK, compiler and dependency source directory:

```sh
uv run python stick/package_runtime.py --build-dir stick/.build/candidate --sdk /path/to/packages/m5stack/tools/esp32s3-libs/3.3.9 --toolchain /path/to/packages/m5stack/tools/esp-x32/2601 --sources-dir /path/to/dependency-sources --output-dir stick/.build/runtime
```

This retains application/library objects, the Arduino core archive, referenced
SDK archives, linker scripts, build configuration, dependency sources and
notices. It creates a portable relinking recipe and verifies that it reproduces
the candidate ELF byte for byte. The [relinking guide](relink.md) explains how
to use it with a modified Arduino runtime.

## Package the release

Commit the final source and documentation, then run from that clean checkout:

```sh
uv run python stick/package_release.py --build-dir stick/.build/candidate --runtime-dir stick/.build/runtime --output-dir stick/.build/release
```

Packaging verifies the source inventory, firmware hashes, production mode,
placeholder artwork, absence of diagnostic markers, infrared timing report and
runtime kit. It produces three archives and `SHA256SUMS`:

- **Firmware ZIP:** application, bootloader, partitions, symbols, installation
  and player guides, manifests, timing result and dependency notices.
- **Source archive:** the committed Stickwalker source and documentation.
- **Runtime archive:** dependency sources and the verified relinking kit.

The firmware manifest records `hardware_qualified: false`. A separate
qualification record identifies the commit, app and ELF hashes and the tests
completed for that image. [Validation history](validation-history.md) provides
the previous device results.

## Device checks for a stable release

Record setup, observations and failures against the frozen candidate. Use
physical buttons for control checks and retail Game Cards for game compatibility.

1. [Back up and install](install.md) the exact candidate. Read back and verify
   the app independently. For an app-only update, confirm registration,
   Pokémon, settings and valid EEPROM mirrors survive.
2. Disconnect USB. Exercise the 500 ms M wake in every layout and rotation,
   ignored dark L/R, abandoned short M taps, navigation after release, Settings,
   display restoration, short sounds and clock progression.
3. Repeat retail walk-start and return transfers on that image. Record
   whole-operation successes and failures, game/version, distance, orientation,
   elapsed time and independent EEPROM verification. Include both HeartGold
   and SoulSilver before reporting tested compatibility with both games.
4. Compare walking and stationary behavior with actual steps and, where
   available, an original Pokéwalker. Measure whole-device battery current with
   USB disconnected and a stated use profile before advertising battery life.
5. Interrupt EEPROM journal/image writes on hardware and verify recovery
   without losing the last completed save.

Use the [debugging guide](debugging.md) for retained diagnostics and USB/JTAG
procedures.

## Publish on GitHub

Run the Linux and macOS CI jobs for the final commit. Check unpacked archives,
installation links and checksums, including a relink from the unpacked runtime
archive. Attach all three archives, checksums and the qualification record.

Write notes for a player: what this version offers, the supported board, how to
install and where to find controls. Include the tested firmware identity and
one clear account of the completed and remaining device checks. Publish early
candidates as prereleases; publish a stable release after its device checks pass.
