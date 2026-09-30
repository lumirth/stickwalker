# Relinking the firmware

This kit is for developers who want to replace or modify the libraries linked
into the release. Installing Stickwalker only requires the firmware ZIP.

The kit contains the release's application and library objects, Arduino core
archive, linked ESP32-S3 SDK archives, linker scripts and dependency source
archives. `sources.json` records their origins and hashes. Licenses and notices
are retained under `licenses/` and in the source archives. Stickwalker's MIT
license does not replace these dependency licenses.

## Reproduce the release ELF

Install the **esp-x32 2601** toolchain through M5Stack's **3.3.9** Arduino board
package. Pass its directory containing `bin/` to the relinker:

```sh
python3 relink.py --toolchain /path/to/packages/m5stack/tools/esp-x32/2601
```

Run this command from the unpacked kit, using Python 3.12 or later. It writes
`output/PwStick.ino.elf` and a linker map. Compare the ELF's SHA-256 with
`runtime-manifest.json`. The release packager verifies that the supplied objects
reproduce the original ELF byte for byte.

## Use a modified Arduino runtime

Unpack `sources/m5stack-3.3.9.zip`. It contains the board configuration, Arduino
core, bundled libraries and build recipes used by the release. Modify that
source in your Arduino package installation, keeping the release's board and
library versions. Build the accompanying Stickwalker source with
`stick/build_port.py --retain-build`, following its build guide.

To link the original application objects with your rebuilt core:

```sh
python3 relink.py --toolchain /path/to/esp-x32/2601 --core /path/to/new-build/relink-build/core/core.a
```

For changes to Arduino's bundled FS, LittleFS or Preferences libraries, replace
the corresponding files in `objects/libraries/` with those from the new build.
The same approach applies to M5 library objects. `link.json` records the ordered
link arguments; SDK archives and scripts are under `sdk/`.

Convert a relinked ELF into an ESP32-S3 flash image with the pinned esptool:

```sh
uvx --from esptool==5.3.0 esptool --chip esp32s3 elf2image --flash-mode dio --flash-freq 80m --flash-size 8MB --elf-sha256-offset 0xb0 -o output/PwStick.ino.bin output/PwStick.ino.elf
```

A modified runtime produces a new candidate. Recheck infrared timing and perform
device qualification before relying on it for game transfers. The release's
hardware evidence applies only to the recorded firmware hashes.

## ESP-IDF sources

The SDK identifies ESP-IDF **v5.5.4**, commit
`735507283d5b2f9fb363a1901172dbd9e847945d`. Its source archive includes the pinned
recursive submodules; `sources.json` records each revision. The Arduino library
builder source and SDK's `versions.txt`, `sdkconfig`, include files and flags are
also included. Linked managed components are LittleFS **1.22.1** and
ESP Diagnostics **1.2.1**. Their component source archives retain their own
notices and build files. Espressif's radio libraries are supplied as upstream
binary archives under their included licenses.
