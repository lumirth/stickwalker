# Device and software validation

The reconstruction, Stick adapters and physical board have been checked at
several levels. These records identify the firmware and setup used for each
result; the current release's qualification record identifies its own tests.

| Area | Evidence and results |
| --- | --- |
| Game connection | A real HeartGold Game Card completed a walk-start transfer, with 374/374 valid optical bursts and independently checked EEPROM mirrors. A subsequent production connection was reported successful. |
| Repeated transfers and storage | A source-faithful 3DS peer completed repeated `back`/`put` operations, including a 20/20 series and exact course readback. The [transfer record](history/transfers.md) identifies the fixtures and retained failures. |
| Sound | Sixteen native scores passed 176 host timing cases. A board trial passed 24/24 acoustic cases across cold starts and warm repeats. See [audio trials](history/audio.md). |
| Power and wake | Battery-only trials exercised sleep returns, advancing RTC, panel restoration and RTC GPIO handoff. Generated gestures exercised the M-only wake policy. See [power and wake trials](history/power-and-wake.md). |
| Production installation | The September 27 readback verified the installed application, preserved paired storage and advancing RTC after resume. |
| Host regressions | Checks cover utility recovery, layouts/chords, wire and EEPROM boundaries, scratch pointers, RTC events, display commands, decoding and adapter lifecycle faults. |

## Optical failures under repeated testing

One `put` trial decoded page 59's first byte as `AB` where the peer sent `AA`;
checksum rejection led to timeout. A subsequent series completed 20/20
operations. A longer diagnostic run completed 44 operations before
`cycle-023-back` decoded `A7` where the peer sent `A6`. Its gate sentinel encoding
does not distinguish a missed pulse from a late sample. The
[transfer record](history/transfers.md#recorded-operations) retains all attempts.

## Reference production image

The saved M-only-wake production application has SHA-256:

```text
5e29d017a468958a07f74da98e4d60602f1e8a28530a742ee5f1bd81631b6c43
```

Its [dated verification](history/power-and-wake.md) records that installation.
The candidate timing check compares the optical sampler with the machine code
from this image. This establishes continuity of acquisition code across builds.

## Completing device qualification

The [release checklist](release.md#device-checks-for-a-stable-release) calls for
physical controls, repeated retail transfers with both games, walking
comparisons, battery measurements and interrupted-write recovery on the exact
candidate. These extend the results above into a stable release test record.

The [evidence archive](history/README.md) retains image identities, measurements,
failed probes and test methods. Raw traces and private flash snapshots are kept
under ignored `stick/.build/`.
