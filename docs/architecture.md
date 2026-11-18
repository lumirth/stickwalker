# How Stickwalker runs the game

## Game and board

The game runs the Pokéwalker menus, pedometer, clock, and cartridge protocol. The board supplies the buttons, motion samples, LCD, speaker, infrared, and flash.

`pokewalker/upstream/` contains the [pinned reconstruction](https://github.com/lumirth/pw/tree/6dc7bc09950078fa3fe0dffa4dae34e9549a99da). The build verifies these files against `pokewalker/upstream.sha256`. `pokewalker/sources.cmake` selects the shared sources and the H8-specific functions that the port replaces. Peripheral drivers live in `pokewalker/port/drivers/`. Port overrides export the original function names so missing replacements fail at link time.

## Infrared

The Stick transmits with its built-in emitter. A BPV10NF photodiode on G5 receives the light pulses. Core 1 samples the floating pin and its discharge under pull-down. The driver keeps separate measurements for light level and pulse timing. Before each connection, it measures 4,096 settled samples and chooses a sampling window and noise allowance. The receiver tracks changes in ambient light during the exchange.

The game receives timed byte and quiet events in `pokewalker/port/drivers/ir.c`. The board pauses LCD transfers during communication. The game replies to the cartridge before committing the completed exchange to flash. The [optical fixtures](../e2e/fixtures/README.md) include recordings of a quiet receiver, a split data pulse, and noise during memory traffic.

## Motion and sleep

The BMI270 samples at 50 Hz during play. With the screen off, it buffers 25 Hz samples while walking. Four seconds of samples use about 600 bytes of its 2,048-byte FIFO. While still, the game checks for activity once a second. Infrared and sound scores can pause motion acquisition.

The port timestamps the buffered samples using the sensor clock, interpolates them onto the game's 16 Hz tick, and advances the calendar between samples. Waking the screen drains the FIFO before the first frame. An overrun restarts the partial FFT. The same pedometer handles a pocket walk and an awake one.

The board can idle at 10 MHz and raises the CPU clock for infrared. Audio powers down 50 ms after a score. The game's ten Shade levels map to backlight PWM values of 2, 5, 10, 18, 28, 42, 64, 96, 155, and 255. The BMI270 shares a 400 kHz I²C bus with the power chip and codec.

When USB is active, the controller stays awake even if the LCD sleeps. The firmware checks for external power once per second and feeds the PM1 watchdog while awake and asleep. At low battery it attempts one save before shutting down.

### Pocket power budget

The motion journey records CPU residency, bus traffic, sleep entries, and flash rail power in `residency.csv`. The model uses 0.240 mA for SoC light sleep. The active CPU ranges are 10–25 mA up to 40 MHz, 22–36.1 mA up to 80 MHz, and 51.2–65.9 mA at 240 MHz. It adds the sensor's mode current and 0.152–0.180 mA for flash and PSRAM standby while their rail is powered. Conversion to battery current assumes 3.7 V and 90% efficiency. Runtime estimates also require usable capacity and a separate estimate of board overhead. Battery life on a physical Stick requires a discharge measurement.

PSRAM is unused. On the StickS3, VDD_SPI can turn off during long light sleeps. The firmware uses an RTC minimum sleep interval of about 3.7 ms for interrupted sleeps and retains the 2.7 ms flash-ready wait on wake. It waits in a task for deadlines under 10 ms. Boards with a different flash supply or added capacitance require separate timing measurements. [ESP-IDF flash power-down](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/system/sleep_modes.html#power-down-of-flash) · [BMI270 datasheet](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf) · [StickS3 schematic](https://docs.m5stack.com/en/core/StickS3)

## Saves and USB

The 64 KiB EEPROM working image lives in internal SRAM. The save service journals changes to 128-byte ranges and checks their CRC. Two flash banks let it replace a checkpoint while retaining the last committed image. An aborted write reloads that image.

`main/management.h` defines the USB commands. Protocol 1 frames requests with SLIP, a request ID, length, and CRC32. Status reads do not borrow the game buffer. Explicit EEPROM reads and writes use bounded pages. An unfinished transfer times out after two seconds. Import and reset are also available while the game is in save recovery.

The host worker owns the selected serial port. A palette edit updates the screen immediately and the Stick saves the settings after 300 ms of inactivity. Palette edits preserve the physical controls selected on the Stick.

## Installation and updates

A routine firmware update flushes the current game and preferences, then flashes the application. The save banks and NVS stay in place. A fresh install initializes both.

For public candidate firmware, the host reads the old LittleFS or raw journal and decodes the EEPROM before writing any overlapping sectors. It stages the EEPROM at the start of the new save partition and writes the partition table last. The new firmware imports the EEPROM on boot. The host translates the old control and display settings and sends them over USB for the firmware to save. A recognizable but damaged legacy save stops a normal install.

Stickwalker Desktop embeds an offline firmware package. Firmware update checks read `firmware-feed/firmware.json` and download the binaries from the matching versioned release. The desktop app has its own update feed. [Release instructions](../crates/sw-desktop/release.md)
