# Building Stickwalker

This creates the firmware you will install on your Stick S3. Download the source
from [the repository](https://github.com/lumirth/stickwalker) and unpack it, or
clone it with Git:

```sh
git clone https://github.com/lumirth/stickwalker.git
cd stickwalker
```

Open a terminal in that folder. Run the following commands on your computer.

Use Python 3.11 or later, [uv](https://docs.astral.sh/uv/), and
[Arduino CLI](https://arduino.github.io/arduino-cli/latest/installation/).
The build was tested with Arduino CLI 1.5.1 on macOS arm64.

Install the tested board core and libraries in your Arduino environment:

```sh
arduino-cli config add board_manager.additional_urls https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
arduino-cli core update-index
arduino-cli core install m5stack:esp32@3.3.9
arduino-cli lib install 'M5GFX@0.2.29'
arduino-cli lib install --no-deps 'M5Unified@0.2.21'
```

Build the firmware:

```sh
uv sync --locked
uv run python stick/build_port.py
```

The builder checks those core and library versions and selects
`m5stack:esp32:m5stack_sticks3:PSRAM=opi`. It generates resident artwork directly
from the included placeholders and compiles production firmware without an
H8 compiler.

Output is `stick/.build/output/`: `PwStick.ino.bin` is the application;
`PwStick.ino.elf` retains symbols for debugging. Bootloader and partition binaries
are generated for first installation. `build-manifest.json` records the board,
tool versions, production/diagnostic mode, artwork, source inputs and output
SHA-256 hashes. Follow the [install guide](install.md) before writing any image.

For an isolated Arduino installation, pass the executable and configuration:

```sh
uv run python stick/build_port.py --arduino-cli /path/to/arduino-cli --config-file /path/to/arduino-cli.yaml
```

For resident retail or edited artwork, follow the [artwork guide](../../assets/README.md),
then add `--artwork local`. Missing local images fall back to placeholders. Keep
that build for personal use. The default build always uses placeholders, even
when `assets/local/` exists.

Use `--output-dir` to keep builds in separate directories. Add `--bench-control`
for a diagnostic build with serial commands and tracing. Release builds use the
default production mode.

## Firmware E2E checks

Install Rust 1.98.1 and Tesseract, then run:

```sh
uv run python stick/check.py
```

This builds production firmware and boots it on a simulated Stick S3. It checks
Settings navigation, rotation and appearance changes, persistence after restart,
and Connect recovery and retry with the receiver held low or high. The runner
reads the rendered display and decodes the transmitted infrared waveform.

Each run saves screenshots, console output, signal recordings, flash and a
result record under `stick/.build/e2e-results/`. The record identifies the
application and simulator used. See the [simulation guide](../simulation/README.md)
for setup, retained builds and scenario commands.

For distributable archives and optical timing verification, follow
[release preparation](release.md).
