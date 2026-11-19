# ![Stickwalker](crates/sw-desktop/assets/wordmark.svg)

Stickwalker runs the [Pokéwalker firmware reconstruction](https://github.com/lumirth/pw) on the M5Stack StickS3. 

For the full write-up and user guide, visit [my website](https://kukas.me/stickwalker/).

## Install

1. Connect a BPV10NF photodiode to the StickS3 with its anode on GND and its cathode on G5.
2. Download [Stickwalker Desktop](https://github.com/lumirth/stickwalker/releases/latest) for your computer.
3. Connect the Stick with a data-capable USB-C cable. Open Stickwalker Desktop and choose **Install Stickwalker**.

## Controls

By default, Power is Left, Front is Center, and Side is Right. Press Front or Side to wake the screen.
Hold Power for 1.5 seconds when the screen is on to open **Stick Settings**. [Button layouts](docs/controls.md) · [Troubleshooting](docs/troubleshooting.md)

## Development

[Build and test](docs/building.md) · [Architecture](docs/architecture.md) · [USB recovery](docs/recovery.md) · [Contributing](CONTRIBUTING.md)

By [lumirth](https://github.com/lumirth). [MIT license](LICENSE) · [Credits](docs/references.md)
