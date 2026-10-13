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

An app-only update keeps your Pokémon, registration, steps, Watts, items and
settings. The October 10 update also repairs missing device IDs on first boot.

Check that the installed partition layout matches the release before updating:

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

Use these esptool commands for updates. Arduino's Upload command also rewrites
the bootloader, partition table and boot selection. If the board previously ran
OTA firmware from app1, inspect its boot selection before using the app0 address.
Stickwalker installations made with this guide use app0.

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
at `0x9000`. On first boot, Stickwalker creates its filesystem in the erased
game-data region.

After boot, check the display and controls, open Settings and test sound. For an
update, confirm the previous registration, Pokémon and settings remain. Unplug
USB, let the screen go dark, and hold M until it wakes, roughly half a second. You can now
[connect to your game and take a Pokémon for a walk](playing.md). The
[controls guide](controls.md) covers button layouts and device settings.

## Infrared receiver

Stickwalker uses the Stick S3's built-in infrared LED to transmit and a
**BPV10NF photodiode** on the expansion header to receive. Connect the photodiode
before trying a Game Card transfer or peer play.

Turn the Stick off and disconnect USB while fitting it:

| BPV10NF lead | Stick S3 header pin |
| --- | --- |
| Anode (A) | **GND** |
| Cathode (C) | **G5** (GPIO5) |

G5 and GND are adjacent on the header. Identify the leads using the
[BPV10NF package drawing](https://www.vishay.com/docs/81503/bpv10nf.pdf#page=4),
especially if they have already been trimmed. Connect the bare photodiode
directly between those two pins. Keep the leads from touching each other or
neighboring pins. Use a connector or mount that holds both contacts firmly;
the connection needs to stay steady while you carry the Stick and during transfers.

Point the photodiode's rounded face and the Stick's infrared transmitter toward
the Game Card's infrared window or the other walker's infrared window. Both
need a clear line of sight. If a transfer times out, check the two connections
and polarity, then adjust alignment and separation while keeping the devices
still.

If transfers work intermittently or report a disrupted connection, turn the
Stick off, disconnect USB and reseat the photodiode. Once both contacts are
secure, turn it on and retry the transfer.
