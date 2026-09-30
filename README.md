# Stickwalker

Stickwalker runs the Pokéwalker application on an **M5StickS3**, with its color
display, buttons, motion sensor, speaker and infrared hardware. It adapts the
[reconstructed firmware](docs/h8-reconstruction.md) to the ESP32-S3 while retaining
the game's state machine, step algorithm and HeartGold/SoulSilver packet format.

The port has completed a walk-start transfer with a real HeartGold Game Card,
and repeated transfers with a source-faithful 3DS test peer. The exact release
image still needs [hardware qualification](stick/docs/release.md); battery runtime
and motion calibration have not been measured. M5StickC and StickC Plus are not
supported by this build.

## Get started

[Build the firmware](stick/docs/build.md), then follow the
[installation and update guide](stick/docs/install.md). The default build uses
original placeholder artwork and requires no ROM or Renesas compiler. You can
[extract resident artwork](assets/README.md) from your own Pokéwalker firmware
for a local build. Course artwork and game data arrive through the console's
normal infrared transfer.

## Controls

The default layout uses M for Left, R for Right, and M+R for Center. Press both
together and hold the overlap for at least 20 ms. L opens Stick Settings.
When the screen is off, **hold M for 500 ms to wake**, then release the buttons
before navigating. L and R become usable after waking.

Stick Settings supports these layouts independently of display rotation:

| Layout | Left | Center | Right | Open Settings |
| --- | --- | --- | --- | --- |
| 2-key M/R (default) | M | M+R | R | L |
| 2-key R/M | R | M+R | M | L |
| 3-key L/R | L | M | R | Hold L for 1.2 s |
| 3-key R/L | R | M | L | Hold L for 1.2 s |

In Stick Settings, M selects a row, R changes it, and L closes the menu. Choose
Light or Dark appearance, rotation, and an 80/120/160 ms two-key chord window;
try audio with the speaker test. Native contrast controls backlight brightness.
Settings persist separately from game data. The battery percentage is an estimate
from voltage, not a remaining-runtime measurement. USB power keeps the processor
awake; battery operation uses Light-sleep between native work deadlines.

## Development

Run `uv run python stick/check.py` for host regressions. See the
[development guide](stick/README.md) for the hardware adapters, protocol boundaries
and validation history. The original H8 build tools remain available through the
[reconstruction guide](docs/h8-reconstruction.md); matching the H8 binary is not a
Stickwalker build requirement.

## Source and license

Original project material is dedicated under [CC0 1.0](LICENSE), with
[third-party exceptions and notices](docs/third-party.md). Retail firmware,
extracted retail artwork, console data and proprietary compiler binaries are
user-provided inputs and are not distributed. Contributions must identify
third-party material and retain its notices; do not submit leaked or confidential
source, ROM dumps or extracted artwork.

This is an unofficial project, unaffiliated with Nintendo, The Pokémon Company,
GAME FREAK, M5Stack or Renesas. See [references and acknowledgments](docs/h8-reconstruction.md#special-thanks)
for the research and community work behind the reconstruction.
