# Stickwalker development

Stickwalker runs the reconstructed Pokéwalker application on M5StickS3.
Start with the [project README](../README.md) for controls and installation.

- [Build and host checks](docs/build.md)
- [Installation and updates](docs/install.md)
- [Release qualification](docs/release.md)
- [Hardware validation history](docs/validation-history.md)
- [Source guide](../docs/source.md)
- [Native motion ownership](../docs/adr/0001-preserve-native-motion-ownership.md)
- [M-only screen-off wake](../docs/adr/0002-m-only-dark-wake.md)

`src/` and `include/` contain the reconstructed application with guarded Stick
compatibility seams. `stick/` provides the display, input, infrared, motion,
storage, clock, audio and power adapters. `src/support/ir.c` owns packet and
session behavior; optical acquisition and decoding belong to `ir_transport.cpp`
and `ir_rx_core.cpp`. Preserve native motion processing and its audio/IR ownership
intervals when changing the scheduler.

A production build omits `PW_STICK_BENCH_CONTROL`. Diagnostic builds can exercise
and change game state through serial commands; keep their output separate from
release firmware. The scripts in `audits/` distinguish host simulation from
on-device measurements. Historical logs, flash backups and received game data
stay in ignored `stick/.build/` and are never release inputs.
