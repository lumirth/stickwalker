# Build Stickwalker

Install [uv](https://docs.astral.sh/uv/getting-started/installation/) and [Arduino CLI](https://arduino.github.io/arduino-cli/latest/installation/). Run these commands from the repository root.

```sh
git clone https://github.com/lumirth/stickwalker.git
cd stickwalker
uv sync --locked
arduino-cli config add board_manager.additional_urls https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
arduino-cli core update-index
arduino-cli core install m5stack:esp32@3.3.9
arduino-cli lib install 'M5GFX@0.2.29'
arduino-cli lib install --no-deps 'M5Unified@0.2.21'
uv run python stick/build_port.py
```

The firmware is in `stick/.build/output/`. Follow [installation](../stick/docs/install.md) to flash it.

## Artwork

Extract the built-in graphics from your own 49,152-byte Pokéwalker ROM, then select them when building.

```sh
uv run -m tools.assets extract /path/to/pokewalker.bin
uv run python stick/build_port.py --artwork local
```

The supported ROM has SHA-256 `f9e210a3b74afbbd12c5a66a51cc05cb9fbac986805ff0a3bfb4be6074d15607`.

For custom graphics, see [editing artwork](../assets/README.md).

## Checks

Install Rust 1.98.1, then run the firmware workflows.

```sh
uv run python stick/check.py
```

For formatting and static analysis, install Cppcheck 2.21.0 and run:

```sh
uv run --group dev -m tools.check
```

## Build the original H8 target

Use Renesas H8 compiler suite 6.02.01 or 6.02.02. Windows x86_64 runs it directly. On macOS Apple Silicon, install Rosetta; configuration supplies Wibo.

```sh
uv run -m tools.toolchain import /path/to/compiler-or-updater
uv run configure.py --toolchain 6.02.01
uv run ninja
uv run ninja verify
```

Extract the original artwork first to reproduce the reference ROM. Repeat with `--toolchain 6.02.02` when changing shared application code.

[Connection reliability](../stick/docs/reliability.md) describes the receiver, storage and qualification limits. [Troubleshooting](../stick/docs/troubleshooting.md) includes the read-only capture command.
