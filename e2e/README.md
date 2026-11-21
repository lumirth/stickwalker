# Test Stickwalker end to end

Build the firmware with the resident artwork. The ESP-IDF installation provides the ESP32-S3 mask ROM ELF. Put two of your own native EEPROM dumps in `e2e/local/`, which Git ignores: `registered.eeprom`, a paired walker with no Pokémon on a stroll, and `course.eeprom`, a walker with a Pokémon on a stroll. [Build instructions](../docs/building.md)

```sh
python3 e2e/journeys.py \
  .deps/idf-tools/tools/esp-rom-elfs/20241011/esp32s3_rev0_rom.elf build/firmware \
  e2e/local/registered.eeprom e2e/local/course.eeprom
```

Independent journeys run in parallel, one per CPU core by default (`--jobs N`).

`src/peer.rs` plays the game's side of a send or return. It follows `pokewalker_ir.c` in the [pokeheartgold decompilation](https://github.com/ctrl-cheeb-del/pokeheartgold): the packet framing, the handshake, the walker identity checks, the 128-byte page transfers, LZ10-compressed course pages, and the 100 ms link timeout.

Use `--output DIRECTORY` to keep the screens and packet traces. Add `--pair BEFORE TRACE PREDICTED` to replay a recorded pairing and compare the resulting save against the captured page writes. Flash timing defaults to typical values. Use `--timing maximum` for the datasheet’s worst-case program and erase times at −40 to 85 °C.

## Saves and USB

`python3 e2e/storage.py` compiles the firmware save code with address and undefined-behavior sanitizers. It cuts power during NOR writes and erases, replaces banks, and checks which EEPROM survives each interruption. It requires a C compiler and zlib. Run `cargo test --locked -p sw-host` for the legacy save fixtures and host protocol tests.

To send USB requests through the emulated controller into the running firmware, run the management journey.

```sh
e2e/run /path/to/esp32s3_rev0_rom.elf build/firmware \
  /path/to/registered.bin management build/desktop-management
```

It exports after a pocket walk, abandons transfers, sends malformed frames, imports over a damaged save, and resets the walker. A palette edit must wake the screen and survive a reboot.

## Play, input, and recovery

The gameplay journey plays Radar and Dowsing using the supplied course’s encounter and item thresholds. It catches a Pokémon, checks the inventory after reboot, and returns the stroll to HGSS. Pairing, send, return, and gameplay compare all 65,536 EEPROM bytes in RAM against the decoded flash.

The palette journey checks Amber in both screen orientations. Rapid Confirm and Back presses exercise the speaker after wake and after the amplifier powers down. Settings closes at different game-tick phases to catch a closing press leaking into gameplay.

Recovery journeys enter the USB downloader through the Power button or USB control lines. They also stall the CPU three times to exercise the watchdog, repair damaged application firmware through the mask ROM, and boot back into the game. Unplugging USB during an optical exchange on a depleted battery must preserve the completed transfer across shutdown.

The board model follows the StickS3 schematic, the PM1 register definitions, and the flash timing table. [StickS3 schematic](https://docs.m5stack.com/en/core/StickS3) · [M5PM1 manual](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_EN.pdf) · [GD25Q64E timing](https://download.gigadevice.com/Datasheet/DS-00484-GD25Q64E-Rev1.6.pdf)

## Infrared

Run `python3 e2e/infrared.py` to replay recorded GPIO5 signals through the production decoder. The fixtures cover an idle line, memory-bus noise, and a data pulse split into two bursts. A separate circuit sweep varies capacitance, pull-down resistance, input threshold, photocurrent, ambient light, and timing. Compare the decoded bytes and silence with the original stream. [Capture provenance](fixtures/README.md)

The peer journey links two firmware instances through their simulated emitters and photodiodes. It checks discovery collisions, gifts, same-day repeat suppression, and the midnight reset. To vary the receiver circuit, set `STICK_IR_PATH=wide-pad` or `STICK_IR_PATH=bright`.

For changes to cartridge discovery, retries, or IR timing, run the HGSS connection scenarios.

```sh
python3 e2e/connections.py .deps/idf-tools/tools/esp-rom-elfs/20241011/esp32s3_rev0_rom.elf \
  build/firmware e2e/local/registered.eeprom e2e/local/course.eeprom --seed 1
```

Hot startup runs on the weak, slow-pad, and bright optical paths and must accept the first reply at a 140 µs turnaround. The other stories use 380 µs, one path each: delayed alignment on weak, a damaged burst on slow-pad, and an absent peer on bright. After a failed transfer, the next attempt must succeed in the same boot. Return starts from the flash produced by send. After the first return, the runner cold-boots to check the pairing and course survive.

Scenarios run in parallel, one per CPU core by default (`--jobs N`), and each result prints as it finishes. `--story hot|delayed|glitch|retry` and `--path weak|slow-pad|bright|nominal`, each repeatable, run every combination of the ones given. The seed controls bus delays and optical noise. A failure preserves traces under `build/connection-failure-*` and prints the seed.

## Buffered motion and pocket energy

After `journeys.py` produces `send/flash.bin`, run the motion comparison.

```sh
python3 e2e/motion.py /path/to/esp32s3_rev0_rom.elf build/firmware \
  /path/to/journeys/send/flash.bin build/motion
```

The runner feeds the same acceleration trace across midnight with frequent timer wakes and four-second batches. It compares total, daily, and archived steps at nominal and ±1.5% sensor clock rates. It also checks pickup after rest, button wake, and an overwritten FIFO. Use `--reference-firmware /path/to/earlier/build` for comparison with another firmware build. Set `STICK_POWER_BASELINE=1` when running `pocket-walk` on firmware that predates FIFO batching.

`residency.csv` records sleep entries, active CPU time, sensor and PM1 traffic, and the fraction of time the flash rail is off. It estimates component current from those observations. The sleep/wake journey presses Front and Side at different times during sleep, checks the panel and USB, and rejects flash power-off intervals shorter than 2 ms. [Power budget and assumptions](../docs/architecture.md#pocket-power-budget)
