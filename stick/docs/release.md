# Preparing a release

A release contains installable firmware, source, runtime dependencies and a test
record identifying the firmware tested. Use a prerelease for early testing;
reserve a stable release for an image that completes the device checks below.

## Build and check

From the intended checkout, run:

```sh
uv sync --locked
uv run python stick/build_port.py --output-dir stick/.build/candidate --retain-build
uv run python stick/check.py --use-build --build-dir stick/.build/candidate
uv run python stick/check_timing.py --elf stick/.build/candidate/PwStick.ino.elf --tool-prefix /path/to/xtensa-esp32s3-elf- --output stick/.build/candidate/ir-timing.json
```

The [build guide](build.md) covers Arduino setup and pinned versions. The
`--retain-build` option saves the objects and link command used by the candidate.
Find `xtensa-esp32s3-elf-` under the Arduino data directory at
`packages/m5stack/tools/esp-x32/2601/bin/`.

The timing check compares `measure_ir_gate`, `aligned_next` and `run_gate_segment`
with the production reference. The report records the new ELF hash and whether
each function's instruction bytes match.

The native-platform redesign changes the receiver handoff and its gate-loop
machine code. The frozen reference is intentionally unchanged. A differing
report is a physical requalification requirement, not permission to replace the
reference with the candidate's hashes. Simulator protocol checks alone cannot
qualify this candidate for a stable release.

## Assemble the runtime kit

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

- The firmware ZIP contains the binaries, symbols, installation and player
  guides, manifests, timing result and dependency notices.
- The source archive contains the committed code and documentation.
- The runtime archive contains dependency sources and the relinking kit.

The firmware manifest records `hardware_qualified: false`. A separate
qualification record identifies the commit, app and ELF hashes and the tests
completed for that image. [Validation history](validation-history.md) provides
the previous device results.

## Device checks for a stable release

Run these checks on the firmware you intend to publish. Record the setup,
observations and failures.

1. [Back up and install](install.md) the candidate. Read back the app and compare
   its hash. Return the Pokémon to HGSS before pre-release updates, initialize
   game data, and confirm device settings, new registration and valid EEPROM
   mirrors.
2. Disconnect USB. Exercise native M-hold wake in every layout and rotation,
   ignored dark L/R, abandoned short M taps, navigation after release, Settings,
   display restoration, short sounds and clock progression.
3. Repeat retail walk-start and return transfers on that image. Record
   whole-operation successes and failures, game/version, distance, orientation,
   elapsed time and EEPROM readback. Test both HeartGold and SoulSilver.
4. Compare walking and stationary behavior with actual steps and, where
   available, an original Pokéwalker. Measure whole-device battery current with
   USB disconnected. Record the time spent walking, idle and using the screen.
5. Interrupt EEPROM journal/image writes on hardware and verify recovery
   without losing the last completed save.
6. Update a paired installation from the September 30 firmware with a Pokémon
   still on it. Confirm identity repair preserves its Pokémon, steps, Watts,
   items and registration, then return the Pokémon to its Game Card.
7. With the [photodiode connected](install.md#infrared-receiver), peer-connect
   with an original Pokéwalker and with another Stickwalker. Check first contact,
   same-day repeat rejection, a different peer and reconnection after day
   rollover. Record each device's firmware and game language.

Use the [debugging guide](debugging.md) for retained diagnostics and USB/JTAG
procedures.

## Publish on GitHub

Run the Linux and macOS CI jobs for the final commit. Check unpacked archives,
installation links and checksums, including a relink from the unpacked runtime
archive. Attach all three archives, checksums and the qualification record.

Write release notes around what changed and how to update. Link the installation
and controls guides. List the tests completed on this build and the device
checks still to run. Publish as a prerelease until the stable-release checks pass.
