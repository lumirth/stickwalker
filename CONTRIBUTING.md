# Contributing

Report bugs and propose changes in [Issues](https://github.com/lumirth/stickwalker/issues). For a device problem, include the firmware version and the steps that reproduce it.

Run the [build checks](docs/building.md#checks) for the code you change. For firmware behavior, extend an existing [simulator journey](e2e/README.md) or describe how you tested on a Stick.

Keep the pinned game sources in `pokewalker/upstream/` intact. StickS3 adaptations belong in `pokewalker/port/`. Keep ROMs, retail artwork, and personal saves out of commits.

Contributions use the [MIT license](LICENSE). Retain [third-party notices](NOTICE.md).

## Reviewing a change

- Which states can occur, and who owns the resource in each one?
- Which transition releases it on failure, disconnection, or cancellation?
- Where is external data bounded and decoded before anything is overwritten?
- After a dialog or USB reset, how do we know the same device can still accept the command?
- Which test exercises the failure path and which platform assumptions still need hardware?
