# Developing Stickwalker

Run the [host checks](docs/build.md#host-checks) before changing the port, then
[build the firmware](docs/build.md). These steps need no board or ROM. Physical
behavior has separate [qualification checks](docs/release.md).

## Find the code

The reconstructed application in `src/` and `include/` owns game rules, records,
and protocol sessions. The adapters here connect that application to M5StickS3.

| Change | Start here |
| --- | --- |
| Scheduling, Settings overlay, foreground ownership | [board_runtime.cpp](board_runtime.cpp) |
| Button acquisition and native control mapping | [input_bridge.cpp](input_bridge.cpp), [controls.cpp](controls.cpp) |
| Panel, palette, rotation and backlight | [display_panel.cpp](display_panel.cpp), [display_bus.cpp](display_bus.cpp) |
| Optical capture, receive decoding and transmission | [ir_transport.cpp](ir_transport.cpp), [ir_rx_core.cpp](ir_rx_core.cpp), [ir_gate.h](ir_gate.h) |
| Motion samples and sensor scaling | [motion_sample.cpp](motion_sample.cpp), [accel_bridge.cpp](accel_bridge.cpp) |
| Audio scheduling and codec supply | [sound_bridge.cpp](sound_bridge.cpp) |
| Game-data persistence and recovery | [eeprom_backend.cpp](eeprom_backend.cpp) |
| Board supplies, PMIC setup and processor sleep | [board_hal.cpp](board_hal.cpp), [power_sleep.cpp](power_sleep.cpp) |

The [player guide](docs/playing.md) describes the complete walk and transfer flow.
The [motion guide](docs/motion.md) explains the sensor adaptation.
The [source guide](../docs/source.md) follows the native application across
modules. [CONTEXT.md](../CONTEXT.md) defines motion, display and power terms.
The [power guide](docs/power.md), [settings/display implementation](docs/device-settings-and-display.md)
and [debugging guide](docs/debugging.md) describe the current adapter contracts.

## Change and validate

Keep protocol/session behavior in `src/support/ir.c`; optical acquisition and
decoding belong in the Stick adapters. Preserve [immediate motion processing
and its ownership gaps](../docs/adr/0001-preserve-native-motion-ownership.md),
and the [M-only screen-off wake policy](../docs/adr/0002-m-only-dark-wake.md).
Changes to the optical sampler need the [machine-code timing check](docs/release.md#build-and-check).

`PW_STICK_BENCH_CONTROL` enables state-changing serial diagnostics. Build it with
`--bench-control` and keep those images separate from production candidates.
The scripts in `audits/` state whether they use host simulation, saved inputs or
a physical board. Logs, flash backups and received data belong in ignored
`stick/.build/`.

Read the [validation summary](docs/validation-history.md) before making hardware
claims. The [evidence archive](docs/history/README.md) preserves dated findings,
including failed trials and superseded designs. For patches and reports, follow
[Contributing](../CONTRIBUTING.md).
