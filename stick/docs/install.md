# Installing and updating Stickwalker

Installing Stickwalker replaces the firmware currently on your Stick S3.
Back it up first so you can restore it if needed.

Download the firmware ZIP from the
[Releases](https://github.com/lumirth/stickwalker/releases)
and unpack it. Check its SHA-256 against the release's `SHA256SUMS`, using
`shasum -a 256` on macOS, `sha256sum` on Linux or `Get-FileHash` in PowerShell.
You can also [build the firmware yourself](build.md).
Use the M5StickS3 USB-C port with a data cable. On macOS, the serial port usually
appears as `/dev/cu.usbmodem*`; on Linux, `/dev/ttyACM*`; on Windows, check the
COM port in Device Manager. Compare the device list before and after connecting
if several ports are present.

Replace `PORT` in the commands below with that port. Run them from the firmware
output directory. [uv](https://docs.astral.sh/uv/) runs the pinned esptool version;
esptool connects to and resets the board.

## Back up before writing

Save the full 8 MiB flash somewhere outside the firmware output directory:

```sh
uvx --from esptool==5.1.0 esptool --chip esp32s3 --port PORT read-flash 0 0x800000 /path/to/stickwalker-before.bin
```

Keep this backup private: it contains registration, Pokémon, game data and device
settings. Check that it is exactly 8,388,608 bytes before proceeding. If the
board won't connect, enter its USB download mode using M5Stack's
[StickS3 instructions](https://docs.m5stack.com/en/core/StickS3).

## Update an existing Stickwalker installation

An app-only update preserves the LittleFS game-data partition and NVS settings.
It requires the existing 8 MB partition layout described below. Read and compare
the installed partition table with the new build before an update:

```sh
uvx --from esptool==5.1.0 esptool --chip esp32s3 --port PORT read-flash 0x8000 0xC00 /path/to/installed-partitions.bin
```

Compare those bytes with `PwStick.ino.partitions.bin`, for example with `cmp`
on macOS/Linux or `fc /b` on Windows. Proceed with the app-only
update only if they match and the existing installation runs from app0:

```sh
uvx --from esptool==5.1.0 esptool --chip esp32s3 --port PORT write-flash 0x10000 PwStick.ino.bin
uvx --from esptool==5.1.0 esptool --chip esp32s3 --port PORT verify-flash 0x10000 PwStick.ino.bin
```

Do not use Arduino's ordinary Upload command for an update: it also writes
bootloader/partition/OTA metadata. This project has no OTA updater. A board that
previously used OTA/app1 needs its boot selection inspected before this procedure.

## First installation

After making a backup, initialize the region where Stickwalker will store its
game data, then install the firmware. This starts a new walker; use the app-only
update above to keep an existing Stickwalker save.

```sh
uvx --from esptool==5.1.0 esptool --chip esp32s3 --port PORT erase-region 0x670000 0x180000
```

Install the bootloader, partition table, boot selection and application:

```sh
uvx --from esptool==5.1.0 esptool --chip esp32s3 --port PORT write-flash 0x0 PwStick.ino.bootloader.bin 0x8000 PwStick.ino.partitions.bin 0xE000 boot_app0.bin 0x10000 PwStick.ino.bin
```

The selected layout has app0 at `0x10000` (size `0x330000`), app1 at `0x340000`,
LittleFS at `0x670000` (size `0x180000`), and coredump at `0x7F0000`. NVS starts
at `0x9000`. First installation changes the partition layout and boot selection;
The erased game-data region becomes a fresh LittleFS filesystem on first boot.
The storage backend also checks writes and journals changes before reporting
them complete.

After boot, check the display and controls, open Settings and test sound. For an
update, confirm the previous registration, Pokémon and settings remain. Unplug
USB, let the screen go dark, and verify a 500 ms M hold wakes it. You can now
[connect to your game and take a Pokémon for a walk](playing.md). The
[controls guide](controls.md) covers button layouts and device settings.
