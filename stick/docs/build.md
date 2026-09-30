# Building Stickwalker

Use Python 3.11 or later, [uv](https://docs.astral.sh/uv/), and
[Arduino CLI](https://arduino.github.io/arduino-cli/latest/installation/).
Release preparation was tested with Arduino CLI 1.5.1 on macOS arm64.

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
from the included placeholders and compiles production firmware. It requires
neither a sibling bench checkout nor the H8 toolchain/build outputs.

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

## Preparing a release candidate

Commit the intended source and run checks, then build and package it:

```sh
uv run python stick/check.py
uv run python stick/build_port.py --output-dir stick/.build/candidate
uv run python stick/check_timing.py --elf stick/.build/candidate/PwStick.ino.elf --tool-prefix /path/to/xtensa-esp32s3-elf- --output stick/.build/candidate/ir-timing.json
uv run python stick/package_release.py --build-dir stick/.build/candidate --output-dir stick/.build/release
```

Find the `xtensa-esp32s3-elf-` tools in your Arduino data directory under
`packages/m5stack/tools/esp-x32/2601/bin/`. The timing check compares the three
optical sampler functions with the frozen production image and binds its report
to the candidate ELF hash.

Packaging accepts a clean committed source tree, production firmware and
placeholder artwork only. It verifies recorded source/output hashes and checks
that diagnostic markers are absent. It creates a source archive, a firmware
archive and checksums. Hardware qualification of those exact bytes remains a
separate [release gate](release.md).
