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

## Host checks

Install a C/C++ compiler with AddressSanitizer and UndefinedBehaviorSanitizer
(Apple Clang on macOS, or GCC/Clang on Linux), then run:

```sh
uv run python stick/check.py
```

This runs the Python utility tests, sanitizer-backed C/C++ tests, and
regressions for peer play, transmission, input, power and codec startup.
They run on your computer with simulated peripherals. The optional optical
replay and HGSS score audits use locally retained recordings and score data.

The peer regressions cover identity provisioning and save migration, first and
repeat encounters, day rollover, reboot, walk return, reset and interrupted
walk-transfer recovery. They exercise the native protocol with a simulated
EEPROM and optical transport.

For distributable archives and optical timing verification, follow
[release preparation](release.md).
