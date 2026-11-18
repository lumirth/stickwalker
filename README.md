# ![Stickwalker](crates/sw-desktop/assets/wordmark.svg)

Stickwalker runs the [Pokéwalker firmware reconstruction](https://github.com/lumirth/pw) on the M5Stack StickS3. [Website and user guide](https://kukas.me/stickwalker/)

## Install

1. Connect a BPV10NF photodiode to the StickS3 with its anode on GND and its cathode on G5.
2. Download [Stickwalker Desktop](https://github.com/lumirth/stickwalker/releases/latest) for your computer.
3. Connect the Stick with a USB-C data cable. Open Stickwalker Desktop and choose **Install Stickwalker**. Updates from earlier public releases keep your save and settings.

Downloads use placeholder built-in artwork. You can [build with the original artwork](docs/building.md#original-artwork) from your own ROM.

## Controls

On a new Stick, Power is Left, Front is Center, and Side is Right. Press Front or Side to wake the screen.
Hold Power for 1.5 seconds to open **Stick Settings**. [Button layouts](docs/controls.md) · [Troubleshooting](docs/troubleshooting.md)

## Development

[Build and test](docs/building.md) · [Architecture](docs/architecture.md) · [USB recovery](docs/recovery.md) · [Contributing](CONTRIBUTING.md)

By [lumirth](https://github.com/lumirth). [MIT license](LICENSE) · [Credits](docs/references.md)
