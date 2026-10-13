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

The registered-state workflows also use `mklittlefs` 4.0.2, installed with the
pinned board core. The runner finds it through the selected Arduino configuration;
`--mklittlefs /path/to/mklittlefs` overrides that discovery. Repeat `--scenario`
to run selected workflows, for example `--scenario controls --scenario wake`.

## Workflows and expectations

`settings.actions` opens Settings with L, uses M and R to change rotation and
appearance, closes the menu and restarts the chip. OCR reads the display in the selected physical orientation before and after
restart. The background brightness also checks the visible appearance. The [controls guide](../docs/controls.md) defines the button
behavior and persistence expected by this workflow.

`connect.actions` selects Connect while GPIO5 is held low. After the attempt
finishes, L opens Settings. It closes the menu, holds GPIO5 high and retries
Connect through M+R. The firmware completes its no-reply sequence and Settings
opens again. A separate decoder checks the emitted light pulses as 115200-baud
8N1 SIR, removes the transport XOR and checks the `0xFC` advertisement on both
attempts. These expectations come from [Dmitry's captured Pokéwalker protocol](https://dmitry.gr/?proj=28.+pokewalker&r=05.Projects). This exercises receiver
sampling, interrupt masking, watchdog service, session cleanup and reuse through
the real firmware.

`controls.actions` boots a public, synthetic registered walker. Thirty-millisecond
contacts navigate Settings, Inventory and Trainer with repeated and alternating
direction taps. A ten-millisecond overlap must remain two directions rather than
become Center. A real M+R chord enters native Settings. Opening the board overlay
while holding R must not change its layout until R is released and pressed again.
The navigation expectations follow `HomeUpdate`, `MainMenuUpdate` and the documented
Stick control mapping, not an injected foreground callback.

`layouts.actions` selects every layout, resets the chip and reopens Settings to
verify persistence. The two three-key layouts must use their respective physical
Left and Right keys to navigate the native menu, reach the explicit third
`STICK SETTINGS` choice and open the overlay with M. Native volume and contrast
remain editable: the workflow changes volume 2 to 0 and contrast 4 to 5, saves,
resets, and checks both checksummed EEPROM mirrors in the resulting filesystem.
The original two-choice behavior comes from `pw_local_settings.c` in the matching
firmware; the third choice is the deliberate Stick extension. Tiny native captions
are checked against the bundled public font, while OCR reads the larger menu labels.
Editor cursor positions are checked independently against the selected values.
After cycling from three-key back to two-key, closing the overlay must restore
the native volume selector rather than leave its third launcher selected. A fresh
dedicated L press must reopen the overlay with the same two-key layout.

`wake.actions` waits for the native 60-second display timeout with USB present.
R and L cannot wake the sleeping panel. A short M press cancels preparation and
releases the shared supply; a qualified M hold wakes the screen and must be consumed
through release. The following two presses navigate normally. This exercises native
eight-scan Center acceptance, not a diagnostic wake function. It does **not** exercise
CPU Light-sleep: USB keeps the CPU awake throughout this workflow.

`battery` runs the same contacts with VBUS at zero. It additionally requires a
retained RTC timer-wake cause, an M-only EXT1 mask while dark, an EXT1 wake for
the accepted M hold, and both front-button pads restored to digital GPIO. These
are hardware-register observations from the model, not firmware diagnostic hooks.
The timer cause is observed at wake rather than an arbitrary later instant: the
RTC model clears that status when the CPU enters its next sleep.

The controls and wake workflows capture actual I2S DMA output from the simulator.
Their generated score contains tempo 120, a six-unit note at pitch index 20 and an
end marker. The matching firmware's compare table gives 77, so its fundamental is
32768/78 Hz. Native duration arithmetic and final-period muting give 48 periods,
or 114.26 ms. Frequency is measured between the first and last complete positive
zero crossing, with two samples of endpoint quantization tolerance. Counting
crossings over the entire amplitude envelope would bias a short note's estimate
downward. Duration tolerance conservatively budgets both edges of the configured
four-by-64-frame DMA queue, one 1 ms service tick and two PCM samples (24.31 ms at
22050 Hz). M5Unified's stop request is consumed by its mixer; it does not discard
already queued samples. This is an acceptance tolerance, not a hardware latency
guarantee. Five navigation notes and the
first cold-wake note plus a warm repeat must all be present separately. These are
digital-output checks, not measurements of acoustic onset, loudness or codec analog
settling.

### Public registered fixture

`fixture.py` generates all 64 KiB from public record layouts and original synthetic
labels rendered with Pillow's default font. It supplies the `nintendo` signature,
valid mirrored identity/status/save records, a registered flag, 100 Watts, no
companion, volume 2 and contrast 4. Its score and simple cursor rasters are generated
locally. It imports no private saves, Nintendo artwork, DS assets or retail scores.

The runner wraps this image in the previous checkpoint format and packs it into a
LittleFS partition sized from the firmware partition table. The simulator loads
that partition before boot; all later migration, reads, writes and NVS changes go
through the real firmware. It never patches game RAM or replaces native callbacks.
The fixture and filesystem-tool hashes are retained with the run's evidence.

Three recovery cases reuse `recovery.actions`, boot normally, navigate the native
menu and reset again. `recovery-stale` publishes a version-two checkpoint containing
200 Watts with an older valid journal containing 100; the older data must not win.
`recovery-legacy` requires a valid legacy journal to update its legacy checkpoint
from 200 to 100 before migration. `recovery-truncated` leaves an incomplete next
append after a 200-Watt checkpoint; that append must not partially apply. Both
native save mirrors must retain their expected big-endian Watts and checksums in
the resulting version-two checkpoint. These constructed crash states test recovery,
not every possible physical flash interruption.

`peer.actions` uses a minimal DS-side protocol peer, not a second copy of the
firmware. It observes GPIO46 SIR, validates checksums and session tokens, sends
status/read requests through GPIO5, and requires the 100-Watt value as `00 64`.
`check.py` separately decodes that reply from the transmitted optical waveform.
`peer-storage.actions` additionally writes one public 128-byte pattern at `0xFF80`,
reads it back, resets the chip and reads it again in a new session. The final
LittleFS checkpoint must contain the same pattern. Both workflows finish by
opening Settings through physical inputs. The peer's ideal charge/discharge
model drives the compiled photodiode receiver; it does not replace decoding or
native protocol callbacks. It is a functional status/page exchange, not a complete
retail DS transfer or an analog interoperability qualification.

Each run creates a fresh directory under `stick/.build/e2e-results/` with screen
images, OCR text, console output, optical waveforms and flash. `results.json`
records the completed workflows and hashes of the application, merged image and
simulator patch. CI retains these files for review, including failed runs.

The suite does not yet establish complete retail transfers, exhaustive power-loss
recovery, walking accuracy, social-offer acceptance, battery life or
physical optical timing. The stationary fixture has no eligible social offer;
observing the wake alone does not prove the offer consequence. An exploratory
battery-only run (`vbus 0`) originally reached an RTC-watchdog reset with cause16
on both unchanged main and the redesign. Guest stacks established that the pinned
RTC model left SLEEP_EN set without generating wake/reject status, trapping the SDK
in `rtc_sleep_start`. The local simulator patch now supplies timer/EXT1 wake,
sleep rejection and watchdog pause during modeled sleep. This enables the battery
workflow but does not measure current consumption or establish physical battery
life.

## Board model and simulator

`board.rs` implements the Stick's I2C1 devices on GPIO47/48, the M5PM1 button
latch, selectable input voltage, stationary BMI270 samples and the ST7789 display on SPI3. Its visible
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
| `run-until-timer-wake 1000` | Run until an RTC timer wake is observed, failing after the given maximum milliseconds |
| `press M`, `release M` | Change a physical M, R or L contact |
| `gpio 5 0` | Hold a GPIO input low or high |
| `serial d` | Send USB serial bytes; used for bench-image investigation, never by production acceptance workflows |
| `screen name.png` | Capture the visible display |
| `optical name.optical` | Save emitted GPIO46 light pulses with durations in CPU cycles |
| `flash name.bin` | Save the flash image |
| `vbus 0` | Set the PMIC input-voltage register; 5000 restores simulated USB power |
| `board name.txt` | Record panel sleep/on state and the shared-supply output latch |
| `audio-clear` | Clear only the host's captured PCM samples, without changing firmware or I2S registers |
| `audio name.pcm` | Save mono signed little-endian 16-bit digital PCM and its `.rate` sample-rate file |
| `peer start` | Enable the ideal optical DS status/Watts peer |
| `peer write-page 65408` | Also write/read a page containing byte `index XOR 0x5A` at the given address |
| `peer read-page 65408` | Also read and verify that page without writing it |
| `peer report name.txt` | Save peer phase, packet trace, observed Watts and any protocol error |
| `peer stop` | Disconnect the test peer before restarting the board |
| `reset` | Restart the chip while retaining flash and board devices |

The simulator executable also accepts an optional initial flash offset and image
after its normal arguments. `check.py` uses this only for the generated LittleFS
fixture, before the ROM boots.
