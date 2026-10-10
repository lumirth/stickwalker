# Developing Stickwalker

The [build guide](docs/build.md) covers toolchain setup, host tests and compiling
firmware. For installing a release on your Stick, use the
[installation guide](docs/install.md).

## Find the code

The reconstructed application in `src/` and `include/` implements game rules, records
and the infrared protocol. The adapters here connect that application to M5StickS3.

| Change | Start here |
| --- | --- |
| Game scheduling and the Settings overlay | [board_runtime.cpp](board_runtime.cpp) |
| Button acquisition and native control mapping | [input_bridge.cpp](input_bridge.cpp), [controls.cpp](controls.cpp) |
| Panel, palette, rotation and backlight | [display_panel.cpp](display_panel.cpp), [display_bus.cpp](display_bus.cpp) |
| Optical capture, receive decoding and transmission | [ir_transport.cpp](ir_transport.cpp), [ir_rx_core.cpp](ir_rx_core.cpp), [ir_gate.h](ir_gate.h) |
| Motion samples and sensor scaling | [motion_sample.cpp](motion_sample.cpp), [accel_bridge.cpp](accel_bridge.cpp) |
| Audio scheduling and codec supply | [sound_bridge.cpp](sound_bridge.cpp) |
| Game-data persistence and recovery | [eeprom_backend.cpp](eeprom_backend.cpp), [device_identity.c](device_identity.c) |
| Board supplies, PMIC setup and processor sleep | [board_hal.cpp](board_hal.cpp), [power_sleep.cpp](power_sleep.cpp) |

The [player guide](docs/playing.md) describes the complete walk and transfer flow.
The [motion guide](docs/motion.md) explains the sensor adaptation.
The [source guide](../docs/source.md) follows the native application across
modules. [CONTEXT.md](../CONTEXT.md) defines motion, display and power terms.
The [power guide](docs/power.md), [settings/display implementation](docs/device-settings-and-display.md)
and [debugging guide](docs/debugging.md) explain the adapters.

## Change and validate

Keep protocol/session behavior in `src/support/ir.c`; optical acquisition and
decoding belong in the Stick adapters. Preserve [the original motion schedule](../docs/adr/0001-preserve-native-motion-ownership.md),
and the [M-only screen-off wake policy](../docs/adr/0002-m-only-dark-wake.md).
Changes to the optical sampler need the [machine-code timing check](docs/release.md#build-and-check).

`PW_STICK_BENCH_CONTROL` enables state-changing serial diagnostics. Build it with
`--bench-control` and keep those images separate from production candidates.
The scripts in `audits/` state whether they use host simulation, saved inputs or
a physical board. Logs, flash backups and received data belong in ignored
`stick/.build/`.

The [test history](docs/validation-history.md) records device trials and the
[release guide](docs/release.md) covers packaging and testing a release. Follow
[Contributing](../CONTRIBUTING.md) for patches and problem reports.

## Device identity and save repair

Each Stick needs a persistent ID for peer play. `device_identity.c` provisions
missing IDs from the ESP32's factory MAC, keeping all six MAC bytes in the
40-byte identity. It preserves existing valid IDs and recovers a damaged master
copy from its backup or saved status.

At mount, the storage backend replays the save journal, repairs the master and
status IDs and the outgoing trainer record, then checkpoints the image before
boot. Repair changes the identity fields and their checksums; the game data
stays intact. `CommitStagedWalk` also copies the current ID into the trainer
record, including when boot finishes a transfer interrupted on older firmware.

The identity and peer tests cover save preservation, interrupted checkpoints,
journal replay, repeat encounters and day rollover. They run with
`uv run python stick/check.py`.
