# Stickwalker

Stickwalker is the firmware that turns an **M5Stack Stick S3** into a recreation
of the **Pokéwalker**, the walking accessory for Pokémon HeartGold and SoulSilver.

The goal is a fully functioning Pokéwalker: take a Pokémon for walks, earn
Watts, play minigames, and transfer Pokémon to and from the games over infrared.

## Try it

You'll need a Stick S3, a USB-C data cable and a computer. There is no published
firmware release yet. To try the current version, [build the firmware](stick/docs/build.md),
then follow the [installation guide](stick/docs/install.md). That guide also
covers backing up your Stick and updating an existing installation.

The default build uses placeholder artwork for the built-in pictures and font.
Pokémon and course artwork arrive from the game during transfer. You can also
[use artwork from your own Pokéwalker firmware](assets/README.md) in a local build.

## Use your Stick as a Pokéwalker

By default, M acts as Left, R as Right, and pressing M+R together acts as Center.
L opens Stick Settings, where you can change the button layout, screen rotation
and appearance, or test the speaker.

Hold M for half a second to wake the screen, then release the buttons before
navigating. The [controls guide](stick/docs/controls.md) explains the other
layouts and how to adjust the M+R timing.

## Development status

Stickwalker is being prepared for release. Earlier firmware has connected
successfully to a real HeartGold Game Card. The current candidate still needs
physical testing, including walking accuracy, battery runtime and retail
SoulSilver transfers. See [what has been validated](stick/docs/validation-history.md)
before deciding whether to try it with your game.

## Help and contribute

[Report a problem or propose a change](CONTRIBUTING.md). If you want to work on
the firmware, start with the [development guide](stick/README.md).
The [original firmware reconstruction](docs/h8-reconstruction.md) is available
for studying how the Pokéwalker works.

## Credits and license

Stickwalker builds on a reconstruction of the original Pokéwalker firmware.
See the [research references and acknowledgments](docs/references.md) for the
work behind it. Original project material is dedicated under [CC0 1.0](LICENSE),
with [third-party exceptions and notices](docs/third-party.md). Retail firmware,
extracted artwork, console data and proprietary compiler binaries are not distributed.

This is an unofficial project, unaffiliated with Nintendo, The Pokémon Company,
GAME FREAK, M5Stack or Renesas.
