# Reliability validation, 2026-10-29

Build `21feec022239` passed 26 compiled-firmware scenarios. Its application SHA-256 is `bdd1dab74b46cc4952f35893b6522db40583b164afb45557a9e762b6a4d281ee`.

| Workflow group | Passed | Coverage |
| --- | ---: | --- |
| UI | 8 | Settings, read-only diagnostics, diode states, correction, both rotations and appearances, bounded probe timeouts |
| Console | 3 | Malformed packets followed by valid traffic, initial upload, return |
| Optical | 4 | Short glitches, leading pulses, a one-byte prefix on full-size packets, slower cartridge timing, compressed pages |
| Regression | 11 | Controls, layouts, screen wake, battery sleep, status/read exchange, update, Walker exchange, storage readback, three storage-recovery cases |

The initial-upload scenario transfers all 281 image chunks plus course and trainer records. Its incoming raster data differs from the preloaded image. The check compares the saved image against that distinct input after commit and reset. Return clears the companion flag while preserving registration and identity.

The malformed-packet workflow rejects a short header, an empty EEPROM-write body, an oversized read, an H8 RAM-write request and a short status record. A following valid read must complete and return the unchanged target page. This workflow exposed a listening gap after packet processing. The corrected sampler resumes after a released event without the no-event tick delay or millisecond alignment wait.

Additional host probes exercised the production decoder with independently generated signals. The raw-wire, checksummed-page and learned-timing sweeps each passed 3,840 cases. An estimated-rate shortcut had rejected 25 valid checksummed packets despite perfect UART framing. Those cases pass with the full unlearned timing search. A separate late-sample probe confirms that removing a recovery tail cannot turn an invalid sample into an accepted idle bit.

The physical app-only update preserved every byte of the device's 65,536-byte EEPROM image. Registration and Pokémon-present flags remained set. The installed production build also acknowledged the versioned read-only diagnostic interface. A passive hardware run detected the inserted diode and completed 69,120 gates without a transport fault.

Formatting, native-target static analysis and simulator formatting checks passed. Preprocessed H8 input for both changed shared C files is byte-identical to the baseline. Both Renesas compilers were unavailable, so their ROM matching builds were not rerun.

The frozen IR instruction hashes changed. The timing baseline remains unchanged and the timing check reports that difference. Active physical reception and real cartridge transfers still need qualification. Simulator instruction costs and flash-busy periods do not establish analog or cache timing on hardware.

Automatic operation restart remains disabled. The exact-source HGSS probe confirms the re-handshake transition, but also demonstrates progress-counter wrap on replay and return completion that can commit on the game side before the Walker receives its final command. See [Connection reliability](../docs/reliability.md#recovery-with-stock-hgss).

To rerun the compiled workflows from the repository root:

```sh
uv run python stick/check.py
```

Each run retains its firmware image, source hashes and evidence under `stick/.build/e2e-results/`. Physical backups and EEPROM exports remain private and outside version control.
