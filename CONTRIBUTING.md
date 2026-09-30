# Contributing

Changes to Stickwalker are welcome. Start with the [development guide](stick/README.md)
for adapter ownership and checks, or the [application source guide](docs/source.md)
for game, storage and protocol work.

## Report a problem

Include your M5StickS3 firmware commit or app hash, what you did, what happened,
and whether USB was connected. For input problems, include layout and rotation.
For infrared problems, include the peer/game, distance, orientation and both
successful and failed attempts. State whether inputs were physical or generated.
A short reproduction and relevant logs help more than a full flash dump.

Flash backups and EEPROM images contain private registration and game data.
Keep them local; share only the diagnostic details needed to reproduce the issue.

## Propose a change

Describe how the patch changes the Stick's behavior as a Pokéwalker and why.
Keep Stick-specific hardware code in `stick/`. Game rules and exchanges with
HeartGold/SoulSilver belong in the reconstructed firmware under `src/`. Preserve
wire/EEPROM widths, byte order and foreground ownership.

Run `uv run python stick/check.py` for Stick changes. Build the target when
changing adapters or shared application code, and identify any physical checks
that remain open. For H8 application changes, also run the formatter, linter,
and retail identity checks under both qualified compiler suites as described
in the [H8 build guide](docs/build.md). Documentation-only changes need working
links and commands that agree with the current tools.

Tests should exercise behavior, recovery or interoperability. Avoid assertions
about wording or source shape unless that text is itself a public contract.

## Source and license

Identify third-party material and retain its notices. Do not submit leaked or
confidential source, ROM dumps, extracted retail artwork or proprietary compiler
binaries. See [third-party notices](docs/third-party.md) for existing exceptions.

Unless otherwise agreed, contributions apply the project's [CC0 terms](LICENSE)
to the copyright and related rights held by their authors.
