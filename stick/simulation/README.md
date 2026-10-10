# Firmware E2E checks

Run the production Stickwalker image on a simulated Stick S3, press its buttons,
and check the screen and infrared output. The ESP32-S3 ROM, bootloader,
FreeRTOS, SDK drivers and application execute together. Flash writes use the
firmware's NVS and LittleFS code.

## Run the checks

Set up the [Arduino build tools](../docs/build.md), Rust 1.98.1 and Tesseract.
On macOS, `brew install tesseract` installs the screen reader. On Debian or
Ubuntu, use `sudo apt-get install tesseract-ocr`.

From the repository root:

```sh
uv sync --locked
uv run python stick/check.py
```

The first run downloads the pinned simulator and Espressif mask ROM, verifies
them, applies `soc.patch`, builds the simulator and builds production firmware.
Later runs reuse the simulator checkout and Cargo output in `stick/.build/simulator/`.

To check a retained firmware build:

```sh
uv run python stick/check.py --use-build --build-dir stick/.build/candidate
```

The runner checks that its source and binary hashes match the build record.
`--arduino-cli` and `--config-file` select a separate Arduino installation when
building through `check.py`.

## Workflows and expectations

`settings.actions` opens Settings with L, uses M and R to change rotation and
appearance, closes the menu and restarts the chip. OCR reads the display before
and after restart. The [controls guide](../docs/controls.md) defines the button
behavior and persistence expected by this workflow.

`connect.actions` selects Connect while GPIO5 is held low. After the attempt
finishes, L opens Settings. It closes the menu, holds GPIO5 high and retries
Connect through M+R. The firmware completes its no-reply sequence and Settings
opens again. A separate decoder checks the emitted light pulses as 115200-baud
8N1 SIR, removes the transport XOR and checks the `0xFC` advertisement on both
attempts. These expectations come from [Dmitry's captured Pokéwalker protocol](https://dmitry.gr/?proj=28.+pokewalker&r=05.Projects). This exercises receiver
sampling, interrupt masking, watchdog service, session cleanup and reuse through
the real firmware.

Each run creates a fresh directory under `stick/.build/e2e-results/` with screen
images, OCR text, console output, optical waveforms and flash. `results.json`
records the completed workflows and hashes of the application, merged image and
simulator patch. CI retains these files for review, including failed runs.

## Board model and simulator

`board.rs` implements the Stick's I2C1 devices on GPIO47/48, the M5PM1 button
latch, stationary BMI270 samples and the ST7789 display on SPI3. Its visible
window crops the controller's GRAM to the Stick's 135-by-240 panel. Chip resets
retain the external devices; their reset commands reset their own state.

The models follow the [M5PM1 manual](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_EN.pdf),
[BMI270 datasheet](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf)
and [Stick S3 wiring](https://docs.m5stack.com/en/core/StickS3#pinmap).
`soc.patch` adds SPI3 using the existing GP-SPI engine, dedicated GPIO matrix
reads, timer-group watchdog stages and RMT waveform capture to
[esp32sim](https://github.com/mikroverk/esp32sim). The
[simulation research](../docs/firmware-simulation-research.md) compares the
available ESP32 testing tools and describes the additional devices and evidence
needed for walking, peer exchange and sleep scenarios.

## Scenario commands

Commands run in order. `run` advances both cores and peripherals for the given
number of milliseconds. Every run stops on an unexpected chip reset or panic.

| Command | Action |
| --- | --- |
| `run 100` | Run firmware for 100 ms |
| `press M`, `release M` | Change a physical M, R or L contact |
| `gpio 5 0` | Hold a GPIO input low or high |
| `screen name.png` | Capture the visible display |
| `optical name.optical` | Save emitted GPIO46 light pulses with durations in CPU cycles |
| `flash name.bin` | Save the flash image |
| `reset` | Restart the chip while retaining flash and board devices |
