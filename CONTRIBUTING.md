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

Run `uv run python stick/check.py` for code changes. It builds the firmware and
executes the E2E workflows.
Describe the device tests you ran. Shared H8 application changes also need the
format, lint and binary-matching checks in the [H8 build guide](docs/build.md).
For documentation, check the instructions and links you changed.

## Tests

Run the compiled firmware through complete user workflows. Drive physical
inputs on the Stick or its simulated board, then check what someone can see,
hear, receive over infrared or recover from saved flash. Establish expectations
from the user contract, protocol specification or independent recordings.

The automated [firmware E2E checks](stick/simulation/README.md) boot the production
image through the ESP32-S3 ROM and its bootloader. Their board models implement
GPIO and bus devices from the hardware specifications. Keep the application and
SDK drivers in that execution path.

Review coverage before adding a test for a bug fix. Extend a relevant workflow
when there is a genuine gap. Remove unit and integration tests,
tautologies and tests that merely detect implementation changes. Expectations
must describe useful behavior and come from evidence independent of the code
being exercised. Apply this review to existing tests too.

## Source and license

Contributions use the project's [MIT license](LICENSE). Credit third-party
material and keep its notices. Keep ROM dumps, extracted retail artwork,
proprietary compiler binaries and confidential source out of contributions.
See [third-party notices](docs/third-party.md) for the included dependencies.
