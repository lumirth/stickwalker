# Stickwalker

Stickwalker turns an **M5Stack Stick S3** into a recreation of the **Pokéwalker**,
the walking accessory for Pokémon HeartGold and SoulSilver. Take a Pokémon for
walks, earn Watts, catch Pokémon and find items, then send your Pokémon and finds
back to the games over infrared.

I built it from my [binary-matching reconstruction of the Pokéwalker firmware](docs/h8-reconstruction.md),
adapting its hardware interfaces to the Stick S3. The reconstructed game code
provides the original rules, progression, minigames and transfer protocol.

## Get started

You'll need a Stick S3, a USB-C data cable and a computer. To connect to your
game, you'll also need an infrared-equipped HeartGold or SoulSilver Game Card
and a console that can run it.

1. Download the firmware ZIP from the [Releases](https://github.com/lumirth/stickwalker/releases).
2. Follow the [installation guide](stick/docs/install.md) to back up your Stick
   and install the firmware.
3. [Connect to your game and take a Pokémon for a walk](stick/docs/playing.md).

The release uses placeholder artwork for built-in pictures and the font.
Pokémon and route artwork arrive from the game during transfer. You can
[build it yourself](stick/docs/build.md), including
[using original artwork from your own Pokéwalker firmware](assets/README.md).

## The Pokéwalker on new hardware

Step counting uses the Pokéwalker's original algorithm. The Stick's Bosch
BMI270 replaces the original Bosch BMA150; Stickwalker converts its readings
to the original scale and sample format and processes them at the original
cadence. Step tracking should therefore behave much like an original
Pokéwalker. [The motion guide](stick/docs/motion.md) explains the adaptation.

The original 96×64 screen is displayed at 2× scale, with light and dark
appearances and adjustable rotation. The speaker plays the original tone
sequences with their reconstructed pitches and note durations. Your Pokémon,
route, registration and progress are saved on the Stick, including across
restarts and app-only firmware updates.

Walking continues with the display asleep. Hold M for half a second to wake
it, then release the buttons before navigating. By default, M moves left,
R moves right and M+R together confirms. L opens Stick Settings. The
[controls guide](stick/docs/controls.md) covers the other layouts and settings.

Earlier firmware has completed transfers with a real HeartGold Game Card.
The [release notes](https://github.com/lumirth/stickwalker/releases/tag/candidate-2026-09-30)
record this version's tests; [validation history](stick/docs/validation-history.md)
contains the detailed device results.

## Help and contribute

[Report a problem or propose a change](CONTRIBUTING.md). For firmware work,
start with the [development guide](stick/README.md). The
[research references and acknowledgments](docs/references.md) credit the research
and tools used in the reconstruction and port.

Stickwalker is an unofficial project by [lumirth](https://github.com/lumirth),
released under [MIT](LICENSE). Dependencies and explicitly marked assets retain
their own [licenses and notices](docs/third-party.md).
