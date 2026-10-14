# Relink a runtime kit

Unpack the runtime archive and run this command from its directory. Use the esp-x32 2601 toolchain supplied by M5Stack's 3.3.9 board package.

```sh
python3 relink.py --toolchain /path/to/packages/m5stack/tools/esp-x32/2601
```

This writes `output/PwStick.ino.elf`. Its SHA-256 should match `runtime-manifest.json`.

To use a rebuilt Arduino core, add `--core /path/to/core.a`.

To create a flash image:

```sh
uvx --from esptool==5.3.0 esptool --chip esp32s3 elf2image --flash-mode dio --flash-freq 80m --flash-size 8MB --elf-sha256-offset 0xb0 -o output/PwStick.ino.bin output/PwStick.ino.elf
```
