# What has been validated

Results apply to the image and setup that produced them. Host tests, saved
optical replay, generated board inputs and physical trials answer different
questions.

| Evidence | Established | Still open |
| --- | --- | --- |
| Host checks | Utility recovery, layouts/chords, serialized boundaries, scratch pointers, RTC events, display commands, decoding and adapter lifecycle faults | Physical behavior, motion calibration and energy |
| Real HeartGold Game Card | Earlier integrated firmware completed walk-start; a later production connection was reported successful | Repeated walk-start/return trials on the exact candidate and a retail SoulSilver trial |
| Source-faithful 3DS peer | Repeated `back`/`put` operations and independent exact course readback | Retail-game qualification and sustained optical reliability |
| Generated board gestures | Scheduler, actual panel/supply restoration and RTC GPIO handoff | Physical-switch behavior for a new image |
| Saved 2026-09-27 production manifest | Installed-app readback, preserved paired storage and advancing RTC after resume | Qualification of a rebuilt candidate |

## Retained failures

One repeated `put` trial decoded page 59's first byte as `AB` where the peer
sent `AA`; checksum rejection led to timeout. A subsequent series completed
20/20 operations. A longer diagnostic run completed 44 operations before
`cycle-023-back` decoded `A7` where the peer sent `A6`. Its gate sentinel encoding
does not distinguish a missed pulse from a late sample. All attempts remain
in the [transfer record](history/transfers.md#recorded-operations).
Successful later trials do not remove these failures.

## Prior production identity

The saved M-only-wake production application has SHA-256:

```text
5e29d017a468958a07f74da98e4d60602f1e8a28530a742ee5f1bd81631b6c43
```

Its [dated verification](history/power-and-wake.md) is the reference for
that installation. A machine-code match for the optical sampler preserves its
known acquisition code, but does not qualify the rest of a new image.

The [evidence archive](history/README.md) retains firmware identities, measured results,
failed probes and test-method limits. Raw logs and flash snapshots are local
ignored inputs. Use the [release checklist](release.md) to record what is still
needed for a candidate.
