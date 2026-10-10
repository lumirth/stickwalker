# Simulating the Stick S3 firmware

The useful test target is the compiled Stickwalker application running on an
emulated ESP32-S3 and a model of the Stick S3. Drive its buttons, acceleration and
infrared receiver, then observe the screen, transmitted packets and persistent
flash. This exercises the application, FreeRTOS, SDK drivers and board adapters
together.

Research checked on 10 October 2026. The selected bring-up target is
[esp32sim](https://github.com/mikroverk/esp32sim), pinned to
`fe3a9c08125cc760107865df3ffa979362952fe9`. It provides editable CPU and peripheral
models, runs locally, and exposes the board events needed by Stickwalker. The local
bring-up now executes Settings and Connect workflows on production firmware.
The runner and setup instructions are in the [simulation guide](../simulation/README.md).

## Simulator choices

| Simulator | Relevant support | Work needed for Stickwalker |
| --- | --- | --- |
| [esp32sim](https://github.com/mikroverk/esp32sim) | Dual-core Xtensa S3; real ROM, bootloader and app; flash/PSRAM; I2C, SPI2, I2S, RMT; editable board models and scripted inputs | Stick S3 board devices, SPI3, dedicated GPIO input and timer-group watchdog behavior |
| [Wokwi](https://docs.wokwi.com/guides/esp32) | S3 CPU, GPIO/IOMUX, flash/PSRAM, I2C, SPI, RMT, LEDC and watchdogs; loads compiled images | Custom Stick S3 devices; S3 I2S is unsupported; dedicated GPIO instruction support needs a runtime probe |
| [Espressif QEMU](https://github.com/espressif/esp-toolchain-docs/blob/main/qemu/README.md) | S3 CPU, ROM, flash/PSRAM, interrupts and timers | Its support table lists I2C, GP-SPI, I2S, RMT and GPIO/IOMUX as unsupported; the existing framebuffer device uses a separate virtual interface |
| [PastorCatto ESP32-Emulator](https://github.com/PastorCatto/ESP32-Emulator/tree/main/qemu) | QEMU patches add S3 SPI2/SPI3, I2C and native USB serial | Board devices and the remaining receiver/audio peripherals; inspect patches because the top-level README predates them |
| [Velxio](https://github.com/davidmonterocrespo24/velxio/blob/master/docs/BUILD-QEMU.md) | Its own QEMU variant adds S3 peripheral callbacks and GP-SPI | Obtain its matching source/runtime archive through a free personal key, then inspect S3 I2S, dedicated GPIO and watchdog behavior |
| [Espressif esp-emu](https://github.com/espressif/esp-emulator) | S3 compiled images, octal PSRAM, GP-SPI, I2C, RMT, GPIO, LEDC and watchdogs; native macOS arm64 binary | Establish how to attach Stick S3 bus devices; the public repository distributes binaries, documentation and tools |

Espressif's [Arduino CI](https://github.com/espressif/arduino-esp32/blob/master/.github/CI_README.md)
uses Wokwi and physical boards for runtime checks. Its current configuration
disables QEMU by default because Wokwi covers more peripherals. M5Stack's
[Stick S3 documentation](https://docs.m5stack.com/en/core/StickS3) supplies the
board wiring, chip identities and firmware flashing routes; the documented
development routes run on the board.

### Wokwi automation

Load a merged image containing the production bootloader, partition table and
application. Set the board attributes to 8 MB flash and 8 MB octal PSRAM, and
`cpuFrequency` to `max`: the default instruction-rate cap is about 8 MHz, while
the optical sampler uses the 240 MHz cycle counter.

[Automation scenarios](https://docs.wokwi.com/wokwi-ci/automation-scenarios)
control buttons and sensors. The [CLI](https://docs.wokwi.com/wokwi-ci/cli-usage)
records serial output, screenshots and logic-analyzer traces; it requires a
Wokwi CI token. The [Python client](https://github.com/wokwi/wokwi-python-client)
also reads display framebuffers and controls simulation start, pause and restart.

[Custom chips](https://docs.wokwi.com/chips-api/getting-started) run as WebAssembly
and can implement I2C/SPI devices. The [time API](https://docs.wokwi.com/chips-api/time)
provides nanosecond deadlines. For the photodiode, model the voltage threshold
crossing after the firmware releases GPIO5; Wokwi's
[analog support](https://docs.wokwi.com/chips-api/analog) is limited, so a generic
photoresistor or 38 kHz remote receiver would model a different circuit.

## Stick S3 model

The production board uses the following devices and routes:

| Device | Connection | Behavior the model must provide |
| --- | --- | --- |
| M5PM1 | I2C1, SDA47/SCL48, address `0x6e` | Register access, supply state, millivolt readings, L-button state and read-cleared press latch |
| BMI270 | Same bus, address `0x68` | Reset/configuration upload, initialization status, data-ready flags and timed acceleration samples |
| ST7789P3 | SPI3; MOSI39, SCLK40, DC45, CS41, reset21 | Display commands, RGB565 RAM, orientation, visible window and status reads |
| ES8311 | I2C address `0x18`; I2S0 MCLK18/BCLK17/WS15/data14 | Codec register state and I2S sample capture, with amplifier enable from M5PM1 GPIO3 |
| M/S buttons | GPIO11/12 | Active-low contacts and timed presses/releases |
| Optical link | Transmitter GPIO46; external photodiode GPIO5 | RMT pulse output and charge/discharge threshold input, including disconnected and loose-contact states |
| Saved data | Production flash partitions | Real erase/write operations, retained data across reset and application updates |

These routes come from `board_hal.cpp`, `sound_bridge.cpp`, `input_bridge.cpp` and
[M5Stack's pin map](https://docs.m5stack.com/en/core/StickS3#pinmap).
Use the [M5PM1 manual](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_EN.pdf)
for defaults and latch semantics, and the
[Bosch BMI270 datasheet](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf)
for sensor configuration and readiness. The Stick S3 has a BMI270; the driver's
optional BMM150 probe does not establish that a magnetometer is fitted.

In the pinned esp32sim source, `DcsPanel::st7789()` already interprets display
writes into GRAM. Add the Stick's visible crop and P3 status replies. Its ES8311
device is currently register memory, so codec power behavior needs its own model.
`BoardModel` exposes cycle-stamped output-enable changes and scheduled input
edges, which fit the photodiode measurement.

The pinned upstream needs three SoC additions for this firmware. SPI3 is named
but unmounted; `ee.get_gpio_in` is an unimplemented Xtensa instruction;
timer-group watchdog registers retain writes without executing watchdog stages.
The repository's `soc.patch` implements these and records RMT pulse durations. CPU timing uses an approximate instruction
cost, with 64-instruction scheduling rounds by default. Functional scenarios can
use this scheduler; optical measurements need comparison with retained hardware
traces before interpreting timing results.

## Extending coverage

Load a retained installation save and update only the application partition.
Read the Pokémon, route and progress through the UI before and after updating.
Inspect the final saved bytes with an independent save decoder, allowing only
the documented identity and checksum changes.

For Connect, drive disconnected, continuously active and intermittent receiver
states. Observe the connection result and continued navigation, then restore a
healthy optical link and complete an exchange. The bring-up comparison below exercises watchdog resets through the compiled
receiver.

Peer scenarios should use separate device identities and persistent saves:
first meeting, repeat meeting, next-day meeting, reboot between encounters and
interrupted walk transfer. Check the visible outcome and independently decoded
transmitted packets/save contents. Retained Pokéwalker recordings and documented
protocol behavior provide expectations; a second copy of Stickwalker supplies a
peer but does not by itself establish interoperability with an original walker.

## Bring-up results

The production Connect candidate boots through the ESP32-S3 ROM and bootloader.
Physical button actions open Settings, change rotation and appearance, restart
the chip and reopen Settings with both choices retained. OCR checks the rendered
screen against the behavior described in the controls guide.

A second workflow holds GPIO5 low during Connect, opens Settings after the
attempt, closes it, holds GPIO5 high and retries. It decodes emitted GPIO46
pulses against [Dmitry's recorded protocol](https://dmitry.gr/?proj=28.+pokewalker&r=05.Projects).
It checks 115200-baud 8N1 SIR framing, the transport XOR and the `0xFC`
advertisement, observes the no-reply sequence and
opens Settings afterward. Both workflows pass on the current candidate with
application SHA-256
`24ef7b4c95c8ccfd31369957cbfe2c98ec1b12c1017bbc32efa49fe0bda67661`.

As a comparison, the released `candidate-2026-10-10` application with SHA-256
`26fa8a557a05083a9df8f9e527cd169c95796e423d4a7267da990a9f005a54c9`
resets during the held-low scenario. This comparison uses the same simulator
models and compiled application images. The low/high inputs exercise electrical
fault and no-reply behavior; retained optical measurements will establish the
charge/discharge model for peer and Game Card exchanges.
