# Build and test Stickwalker

Run the commands from the repository root. The Rust workspace contains the desktop app and its USB host library. The firmware uses ESP-IDF for the ESP32-S3.

## Firmware

Use the pinned ESP-IDF checkout. If you already have that SDK, set `IDF_PATH` and `IDF_TOOLS_PATH` and skip the installation.

```sh
git clone https://github.com/espressif/esp-idf.git .deps/esp-idf
git -C .deps/esp-idf checkout fcae32885b0296b32044cb99ecbdc50d98dddb83
git -C .deps/esp-idf submodule update --init --recursive
IDF_TOOLS_PATH="$PWD/.deps/idf-tools" .deps/esp-idf/install.sh esp32s3
tools/build.sh build
python3 tools/package.py firmware
```

In Stickwalker Desktop, choose **Device → Open Firmware…** and open `dist/firmware/firmware.json`. The manifest and its three binaries belong together.

### Flash a local build over USB

Use Desktop to migrate a save from an earlier Stickwalker preview. To write a local build directly to a Stick with the current partition layout, put it in download mode and set `PORT` to its serial port. Run esptool in the ESP-IDF Python environment. [USB recovery](recovery.md)

```sh
cd build/firmware
python -m esptool --chip esp32s3 --port "$PORT" \
  --before no_reset --after watchdog_reset write_flash @flash_args
```

## Original artwork

The default build includes placeholder artwork. To use the built-in graphics from your own 48 KiB Pokéwalker ROM, extract them locally and rebuild.

```sh
python3 tools/extract_resident.py /path/to/pokewalker-rom.bin
tools/build.sh -DSW_RESIDENT_ASSETS_HEADER="$PWD/assets/local/rom_assets.h" build
python3 tools/package.py firmware
```

## Desktop

Run `cargo run --locked -p sw-desktop`. In a debug build, press Cmd/Ctrl+1 through 8 to preview the device states and Cmd/Ctrl+0 to return to the connected Stick.

On Windows and Linux, run `python3 tools/package.py desktop` to package a build. On macOS, install the Finder disk image tools first.

```sh
python3 -m venv build/dmg-tools
build/dmg-tools/bin/pip install -r tools/requirements-macos.txt
build/dmg-tools/bin/python tools/package.py desktop
```

## Checks

```sh
cargo clippy --locked --workspace --all-targets -- -D warnings
cargo test --locked --workspace
python3 e2e/storage.py
python3 e2e/infrared.py
```

The storage check needs a C compiler with address and undefined-behavior sanitizers and zlib. The [simulator journeys](../e2e/README.md) run the compiled firmware through game, USB, infrared, and power transitions. See [release instructions](../crates/sw-desktop/release.md) for signing and distribution.
