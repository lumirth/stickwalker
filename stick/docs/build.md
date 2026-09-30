# Building Stickwalker

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

From the repository root:

```sh
uv sync --locked
uv run python stick/build_port.py
```

The builder checks those exact core/library versions and selects
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

Use `--output-dir` to keep candidates separately. `--bench-control` enables
state-changing serial diagnostics; it is never a release option.

## Host checks

Install a C/C++ compiler with AddressSanitizer and UndefinedBehaviorSanitizer
(Apple Clang on macOS, or GCC/Clang on Linux), then run:

```sh
uv run python stick/check.py
```

This runs the Python utility tests, twelve sanitizer-backed C/C++ executables,
and five transmitter/input/power/codec lifecycle regressions. It needs no board,
ROM, console-source checkout or Renesas compiler. Saved optical replay and native
HGSS score audits are separate optional checks with local evidence inputs.

For distributable archives and optical timing verification, follow
[release preparation](release.md).
