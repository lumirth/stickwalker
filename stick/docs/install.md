# Install Stickwalker

Download and unpack the firmware ZIP from [Releases](https://github.com/lumirth/stickwalker/releases). Run the commands below from the unpacked folder. They use [uv](https://docs.astral.sh/uv/getting-started/installation/) to run esptool.

Connect the Stick S3 with a USB-C data cable. Replace `PORT` with its serial port, usually `/dev/cu.usbmodem*` on macOS, `/dev/ttyACM*` on Linux or a `COM` port on Windows.

## Photodiode

The Stick's built-in IR LED transmits. Add a BPV10NF to receive.

| BPV10NF | Stick S3 |
| --- | --- |
| Anode | GND |
| Cathode | G5 |

Use the [package drawing](https://www.vishay.com/docs/81503/bpv10nf.pdf#page=4) to identify the leads.

## Back up

```sh
uvx --from esptool==5.1.0 esptool --chip esp32s3 --port PORT read-flash 0 0x800000 stickwalker-backup.bin
```

## Update

Return your Pokémon to HGSS first. Pre-release updates reset registration and walker progress. Button layout, rotation and appearance stay saved.

```sh
uvx --from esptool==5.1.0 esptool --chip esp32s3 --port PORT --after no-reset erase-region 0x670000 0x180000
uvx --from esptool==5.1.0 esptool --chip esp32s3 --port PORT write-flash 0x10000 PwStick.ino.bin
```

## First installation

```sh
uvx --from esptool==5.1.0 esptool --chip esp32s3 --port PORT --after no-reset erase-region 0x670000 0x180000
uvx --from esptool==5.1.0 esptool --chip esp32s3 --port PORT write-flash 0x0 PwStick.ino.bootloader.bin 0x8000 PwStick.ino.partitions.bin 0xE000 boot_app0.bin 0x10000 PwStick.ino.bin
```

[Stick controls](controls.md).

If esptool cannot connect, use M5Stack's [USB download-mode instructions](https://docs.m5stack.com/en/core/StickS3).
