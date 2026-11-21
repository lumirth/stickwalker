# How Stickwalker runs the game

## Game and board

The game runs the Pokéwalker menus, pedometer, clock, and cartridge protocol. The board supplies the buttons, motion samples, LCD, speaker, infrared, and flash.

`pokewalker/upstream/` contains the [pinned reconstruction](https://github.com/lumirth/pw/tree/6dc7bc09950078fa3fe0dffa4dae34e9549a99da). The build verifies these files against `pokewalker/upstream.sha256`. `pokewalker/sources.cmake` selects the shared sources and the H8-specific functions that the port replaces. Peripheral drivers live in `pokewalker/port/drivers/`. Port overrides export the original function names so missing replacements fail at link time.

## Infrared

The Stick transmits with its built-in emitter. A BPV10NF photodiode on G5 receives the light pulses. Core 1 samples the floating pin and its discharge under pull-down. The driver keeps separate measurements for light level and pulse timing. Before each connection, it measures 4,096 settled samples and chooses a sampling window and noise allowance. The receiver tracks changes in ambient light during the exchange.

The game receives timed byte and quiet events in `pokewalker/port/drivers/ir.c`. The board pauses LCD transfers during communication. The game replies to the cartridge before committing the completed exchange to flash. The [optical fixtures](../e2e/fixtures/README.md) include recordings of a quiet receiver, a split data pulse, noise during memory traffic, and saturating flashes from a DS held against the Stick.

A flash strong enough to discharge the pin within the floating phase saturates the receiver, and the diode keeps conducting briefly afterwards. The decoder treats light that only fades after such a flash, or that can be neither the same flash nor the next bit's, as recovery. That rule uses only the protocol's timing and whether the line itself saturates, so it does not depend on a particular emitter, diode or Stick.

CPU1's sampler times the pin by counting instructions. CPU0 fetching code from IRAM at the same time delays those instructions, so the decoder runs from flash and the game's clock counts CPU cycles while the receiver is open. Calibration waits between polls of the sample ring for the same reason.

`e2e/optical_channel.c` sweeps the receiver circuit, ambient light, clock error and these recovery tails. It assumes peers send IrDA SIR timing; their brightness spans weak flashes to sixty times a DS measured at contact. A tail that keeps the pin saturated until the next bit can begin carries no information, so those cases only have to produce no false bytes. `STICK_RANGE=1` reports the distances at which the weakest and strongest IrDA transmitters decode.

## Motion and sleep

The BMI270 samples at 50 Hz during play. With the screen off, it buffers 25 Hz samples while walking. Four seconds of samples use about 600 bytes of its 2,048-byte FIFO. While still, the game checks for activity once a second. Infrared and sound scores can pause motion acquisition.

The port timestamps the buffered samples using the sensor clock, interpolates them onto the game's 16 Hz tick, and advances the calendar between samples. Waking the screen drains the FIFO before the first frame. An overrun restarts the partial FFT. The same pedometer handles a pocket walk and an awake one.

The board can idle at 10 MHz and raises the CPU clock for infrared. Audio powers down 50 ms after a score. The game's ten Shade levels map to backlight PWM values of 2, 5, 10, 18, 28, 42, 64, 96, 155, and 255. The BMI270 shares a 400 kHz I²C bus with the power chip and codec.

When USB is active, the controller stays awake even if the LCD sleeps. On battery, the firmware checks for a cable during scheduled motion wakes, about every two seconds at rest. Voltage sampling and the watchdog share a 30-second interval in the pocket. At low battery it attempts one save before shutting down.

### Pocket power budget

The motion journey records CPU residency, bus traffic, sleep entries, and flash rail power in `residency.csv`. The model uses 0.240 mA for SoC light sleep. The active CPU ranges are 10–25 mA up to 40 MHz, 22–36.1 mA up to 80 MHz, and 51.2–65.9 mA at 240 MHz. It adds the sensor's mode current and 0.152–0.180 mA for flash and PSRAM standby while their rail is powered. Conversion to battery current assumes 3.7 V and 90% efficiency. Use a discharge measurement for the board’s actual runtime.

PSRAM is unused. On the StickS3, VDD_SPI can turn off during long light sleeps. The firmware uses an RTC minimum sleep interval of about 3.7 ms for interrupted sleeps and retains the 2.7 ms flash-ready wait on wake. It waits in a task for deadlines under 10 ms. The flash supply and its capacitance determine the minimum safe wake delay. [ESP-IDF flash power-down](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/system/sleep_modes.html#power-down-of-flash) · [BMI270 datasheet](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf) · [StickS3 schematic](https://docs.m5stack.com/en/core/StickS3)

## Saves and USB

The 64 KiB EEPROM working image lives in internal SRAM. The save service journals changes to 128-byte ranges and checks their CRC. Two flash banks let it replace a checkpoint while retaining the last committed image. An aborted write reloads that image.

`main/management.h` defines the USB commands. Protocol 1 frames requests with SLIP, a request ID, length, and CRC32. Status reads run without pausing the game. Explicit EEPROM reads and writes use bounded pages. An unfinished transfer times out after two seconds. Import and reset are also available while the game is in save recovery.

The host worker owns the selected serial port. A palette edit updates the screen immediately and the Stick saves the settings after 300 ms of inactivity. Palette edits preserve the physical controls selected on the Stick.

Firmware and Desktop update independently, so neither may require the other to update:

- Desktop changes only firmware, saves and palettes. Everything else is set on the Stick, and Desktop carries the Stick's own settings through without interpreting their values.
- The status reply keeps its first 225 bytes. New fields go after them, and Desktop ignores bytes it doesn't know.
- A new Stick-only setting gets its own NVS key, not a field in the 151-byte settings block.
- A capability Desktop needs from firmware is a new command. Firmware answers commands it doesn't know with an error, which Desktop reads as "not supported".

## Installation and updates

A routine firmware update flushes the current game and preferences, then flashes the application. The save banks and NVS stay in place. A fresh install initializes both.

For earlier Stickwalker previews, the host reads the old LittleFS or raw journal and decodes the EEPROM before writing any overlapping sectors. It stages the EEPROM at the start of the new save partition and writes the partition table last. The new firmware imports the EEPROM on boot. The host translates the old control and display settings and sends them over USB for the firmware to save. A recognizable but damaged legacy save stops a normal install.

Desktop checks `firmware-feed/firmware.json` for new releases. It downloads the images from the versioned firmware release, verifies their hashes and flash layout, and keeps that one package in the system cache folder for offline installs. When the feed changes, even back to an older release, Desktop replaces the cached package. Open Firmware installs a local package once and leaves the online release in place. Desktop offers the online release only when it is newer than the installed firmware, so a release replaces dev builds of the same version. Desktop app updates use a separate feed. [Release instructions](../crates/sw-desktop/release.md)

## Power controller recovery

Stickwalker writes `0x1f` to PM1 register `0x49` to set the Power button timing. A previous configuration can leave the download lock latched. On affected revisions the lock also blocks the `SYS_CMD=0xA3` download request. Resetting the ESP32 alone leaves the power controller running.

On HW5 with PM1 revision `0x4f`, restarting the PM1 application from M5Stack's updater cleared the lock. The ESP32's USB downloader remains accessible over a working USB connection. [PM1 register reference](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_EN.pdf) · [USB recovery](recovery.md)
