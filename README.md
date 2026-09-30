# Stickwalker

Play the Pokéwalker application on an **M5StickS3**. Stickwalker brings its
walking progression, menus, minigames and infrared transfers to the Stick's
screen, buttons, motion sensor and speaker.

This is a release candidate. Earlier firmware completed a walk-start transfer
with a real HeartGold Game Card and repeated transfers with a 3DS test peer.
The candidate still needs physical qualification; walking calibration and
battery runtime are unmeasured. See the [validation summary](stick/docs/validation-history.md).
The supported board is M5StickS3.

## Get started

[Build Stickwalker](stick/docs/build.md), then [install it](stick/docs/install.md).
The default build includes original placeholder artwork and needs no ROM or
Renesas compiler. You can use [your own resident artwork](assets/README.md)
for a local build. Course artwork and game data arrive through the game's
normal infrared transfer.

M acts as Left, R as Right, and M+R as Center. Press M and R together and hold
both for at least 20 ms. L opens Stick Settings, where you can change the
layout, rotation, appearance, and test the speaker.

When the screen is off, **hold M for 500 ms to wake**, then release the buttons
before navigating. See [controls and settings](stick/docs/controls.md) for the
other layouts and the Center timing adjustment.

## Work on Stickwalker

Start with the [development guide](stick/README.md) to find the hardware
adapters and run checks. [Contributing](CONTRIBUTING.md) explains how to report a
problem or propose a change. The [H8 reconstruction](docs/h8-reconstruction.md)
is also available for studying and reproducing the original firmware.

## Credits and license

Stickwalker adapts the reconstructed Pokéwalker firmware. See the
[research references and acknowledgments](docs/references.md) for the work behind it.
Original project material is dedicated under [CC0 1.0](LICENSE), with
[third-party exceptions and notices](docs/third-party.md). Retail firmware,
extracted artwork, console data and proprietary compiler binaries are not distributed.

This is an unofficial project, unaffiliated with Nintendo, The Pokémon Company,
GAME FREAK, M5Stack or Renesas.
