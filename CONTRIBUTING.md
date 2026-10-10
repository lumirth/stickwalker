# Contributing

For firmware changes, start with the [Stick development guide](stick/README.md)
or the [application source guide](docs/source.md).

## Report a problem

Include the release version, or commit for a custom build, what you did, and
what happened. A short sequence that reproduces the problem is especially
useful. Also include the details relevant to it:

- For buttons or wake, the layout, rotation and whether USB was connected.
- For infrared, the photodiode wiring, other device or game, distance and
  orientation. Include how often the connection succeeds or fails.
- For a test script, the command and relevant output.

Keep flash backups and EEPROM images private; they contain registration and
game data. Share the diagnostic output needed to describe the problem.

## Propose a change

Explain the problem and how the patch changes the Stick's behavior. Stick
hardware adapters live in `stick/`; game rules and the infrared protocol live
in `src/`. Preserve stored record sizes, byte order and the order in which tasks
use shared working memory.

Run `uv run python stick/check.py` and build the firmware for code changes.
Describe the device tests you ran. Shared H8 application changes also need the
format, lint and binary-matching checks in the [H8 build guide](docs/build.md).
For documentation, check the instructions and links you changed.

Add tests for the behavior being changed, including relevant failure and
recovery cases.

## Source and license

Contributions use the project's [MIT license](LICENSE). Credit third-party
material and keep its notices. Keep ROM dumps, extracted retail artwork,
proprietary compiler binaries and confidential source out of contributions.
See [third-party notices](docs/third-party.md) for the included dependencies.
