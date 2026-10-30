# Calibration-free IR replacement

This work replaces the Stick S3 link implementation from
`3e6634d19201d2f58507af2d0c479a41b79ad533`. Its requirement is a single fixed
receiver which needs no per-unit calibration, idle training interval or learned
baud estimate. The original H8 application remains the protocol owner.

## What was removed

The old `ir_transport`, `ir_rx_core`, `ir_tx`, `ir_gate` and
`ir_packet_validation` implementations were deleted. Their instruction-identity
baseline and receiver replay wrapper were also removed. There is no alternate
legacy path or runtime selection between receivers.

The nine replaced implementation/header files contained 1,492 lines. Their
five replacements contain 1,202 lines, a reduction of 290 lines (19.4%), before
counting the deleted replay wrapper and obsolete timing baseline. Test-model
extensions and evidence documentation are separate from this production-code
comparison.

The replacement removes these assumptions:

| Previous assumption | Replacement contract |
| --- | --- |
| An absolute crossing-delay cutoff identifies optical activity. | Every observation is retained while listening; local contrast identifies candidate pulses. |
| No crossing can be represented by a large numerical sample. | Finite crossing upper bounds, censored lower bounds and invalid measurements have distinct representations. |
| Optical burst completion grants permission to stop sampling. | Only native admission of a handled request, a timeout, or termination grants a service turn. |
| A packet can supply a fitted baud and timing origin. | Each UART byte supplies its own start origin on one fixed sampling schedule. |
| Task or ISR completion marks the end of local light. | The sampler observes the sticky RMT hardware completion event and retains input across it. |
| A flash append below the receive timeout proves timely service. | Diagnostics measure accepted request/reply and completed-reply intervals; storage ownership is checked separately. |
| Preserving the old gate's instruction bytes preserves correctness. | The complete acquisition owner is inspected and exercised as compiled code; physical timing still requires measurement. |

## Acquisition and decoding

`ir_link.cpp` owns GPIO phases, a 32,768-entry internal-RAM observation ring,
publication, the TX boundary and bounded receive turns. Its single IRAM loop
uses the electrical integration interval for publication and ownership work.
It reserves ring space before writing and publishes only completed samples.
The consumer releases samples after copying them into private decoder state.
Overruns fail before unread input is overwritten.

At the required 240 MHz CPU clock, the fixed schedule alternates 694 and 695
cycles for 345,600 observations per second. Release and readout are scheduled
at offsets 65 and 450; threshold polling stops against the next origin minus
70 cycles. Each probe checks that cutoff before reading GPIO. The turn's
independent 140 ms wall deadline is checked inside each integration interval.

Each 16-bit observation contains a finite delay or censored lower bound, plus
explicit flags for censoring and invalidity. The first observation after a
service interval marks a discontinuity. A late electrical phase is an erasure;
it cannot silently become a plausible dark sample.

`ir_receiver.cpp` uses a nine-observation upper envelope with four gates of
lookahead and a fixed six-cycle contrast floor. Censored background can provide
a conservative reference, but a censored current sample cannot prove a pulse.
This is a local signal operation, not calibration state. The decoder needs no
idle history and never uses command expectations or checksums to choose bits.

UART framing uses three gates per bit and a fresh origin for every byte. An
actual framing contradiction can retry a bounded history of pulse starts.
The receiver keeps only the last 136 bytes and uses the original wrapping
Timer W timestamps for frame separation. Noise which itself forms valid UART
is left for the independent native protocol to reject.

## Native ownership, storage and transmission

`ir_packet.h` describes whether a message is rejected, leaves reception open,
or invokes a native handler. Command shape, session state and checksum are
independent of optical decoding. A valid complete packet takes precedence over
packet-shaped bytes in its payload; otherwise exactly one valid suffix can
remove a noise prefix. The old special case for leading `FF` bytes is gone.

Ignored `0x2E` traffic, duplicate establishment replies and unrelated page ACKs
leave acquisition running and do not renew the receive deadline. A normal turn
has a fixed 3,200-tick receive limit and one fixed 685-tick allowance for
an arriving frame. The physical owner also enforces an independent wall-clock
limit. Indefinitely extended peer-only streams are deliberately unsupported.

Before a handled request enters flash code, the producer parks and restores
interrupts. Journal appends and checkpoint entry check this ownership even
after a failed stop. The EEPROM format, recovery journal and native completion
actions retain their existing semantics. Page writes remain durable before
their page acknowledgement.

Transmission pre-encodes the complete maximum 136-byte frame into the RMT DMA
transaction. The sampler is armed before launch. Its hardware TX-end fence
retains the final dark stop observations and the following incoming samples,
including a reply starting before the driver callback or foreground task runs.
No optical activity threshold or post-TX quiet timer discards this boundary.

## Verification method

The authoritative workflow executes the actual production ESP32-S3 image,
drives its GPIO and optical inputs, decodes its external outgoing light, and
compares transferred and persisted data with independent public fixtures.
Receiver samples and decoded bytes are not injected into the firmware.

The existing simulator was extended to model RMT DMA descriptor/FIFO progress
and an optional continuous RC circuit. Its inputs are resistance, capacitance,
input threshold, signal photocurrent and background photocurrent. It knows
nothing about the receiver's gates or acceptance decisions. The original ideal
optical model remains, including its censored dark measurements. The same
opcode, branch, call and load-use costs apply to both models. See the
[model documentation](../simulation/README.md) for assumptions and scenarios.

The added complete-transfer cases cover finite RC baselines, narrow pulses,
clock error, interbyte gaps, a 1 microsecond reply with local echo, a maximum
frame, a short handshake crossing the native timeout, and ignored packets
followed immediately by a real request. A separate ignored-only train checks
that reception terminates at its fixed deadline. The storage workflow includes
payload bytes which also form a valid packet, with exact readback before and
after reboot.

An initial deadline test delayed a 136-byte page until 86.2 ms after the previous
reply. The receiver delivered it correctly at approximately 98.234 ms, but this
left an impossible service budget under the existing flash model. A 128-byte
page creates a 132-byte journal patch, written in at least three 64-byte NOR
program operations, plus separate header and commit programs. Five 300 us
programs and an eight-byte ACK require at least 2.194 ms, before CPU, readback
and arming costs. The remaining 1.766 ms cannot accommodate that work. No peer
deadline or flash latency was relaxed. The deadline scenario instead delays
the initial eight-byte FA by 97.2 ms: it finishes after the native 97.656 ms
receive deadline, and its no-storage F8 response can finish before the peer's
unchanged 100 ms limit. Maximum-page durability remains covered at ordinary
turnaround in the complete-transfer scenarios.

Compiled execution exposed two acquisition scheduling defects during the
replacement: publication work at every 32nd gate, then a dark polling batch
which could overshoot its read limit and consume teardown time. Publication
moved inside integration; probing now checks the deadline before each GPIO
read. The strict invalid-measurement checks and simulator timing were retained.
Independent review also corrected TX-fence publication ordering, ring block
ownership, and the stop-failure/storage path.

Two existing UI assertions depended on the old observation timing. The
advisory-absence snapshot occurs during active reception, when the pin may be
in its recharge phase; only that snapshot permits an enabled output, while
post-attempt and unsafe-input checks still require release. The no-peer check
now observes the failed-result-to-home sequence, requiring the uncertain icon
on the result and its disappearance at home. It no longer assumes the result
is still open at one fixed late screenshot time. No production UI behavior was
changed to satisfy those checks.

## Candidate and reproduction

The retained production candidate uses Arduino CLI 1.5.1, M5Stack ESP32 core
3.3.9, M5Unified 0.2.21 and M5GFX 0.2.29, with bench control disabled.

| Item | Identifier |
| --- | --- |
| Build ID | `a97b87ae69cf` |
| Application SHA256 | `f8096c0c2e5415a0e92782be0f1df05535893167a83a8073d9979d6d275e100d` |
| ELF SHA256 | `9449214d028aeeda9e9c9847bfb6972271a27a703cb61d8d19a92aa4d0b323d1` |
| Acquisition function SHA256 | `e60f95ee9f9c4b7530e15aea396dfe69c09f86aa8d6bd969926bb2ce95d60624` |

The acquisition function occupies 559 bytes entirely in IRAM, with no direct or
indirect calls. The compiled dark-readout path has at least 35 cycles before the
next gate in the instruction-cost model. A single late startup observation was
located at gate zero, already explicitly invalid and before initial FC. It was
not delivered as peer data. This does not assert zero timing error on hardware.

The full verification workflow contains 37 scenarios. Its final completion
status and retained result identifiers are recorded in the pull request; an
interrupted or partial run is not a completed pass. Each run records the image,
firmware-source, verification-source and simulator hashes, and the CI coverage
checker requires matching provenance across all five shards.

With the pinned tools installed as described in [the build guide](../../docs/build.md):

```sh
python stick/build_port.py --output-dir stick/.build/e2e-firmware
python stick/simulation/bootstrap.py
python stick/check.py --use-build --use-prepared-simulator --shard 1/5
```

Run shards `2/5` through `5/5` for the other assigned scenarios, or omit `--shard`
to execute the complete suite. The formatting/static-analysis command is
`python -m tools.check`. The shared `src/support/ir.c` was also preprocessed
without `PW_STICK_S3` and compared with the base revision: its normalized native
tokens are unchanged. The proprietary H8 compiler/binary comparisons were not
run in this environment.

## Qualification boundary

Simulation establishes behavior for its specified inputs. The RC profiles are
illustrative inputs, not manufacturer-guaranteed tolerance limits. They do not
model measured noise, nonlinear diode capacitance, optical alignment, GPIO
latency or DMA/cache arbitration on the device.

The fixed schedule and six-cycle floor still need a common physical margin:
the smallest supported pulse must remain distinguishable from acquisition
error and local background variation on every supported unit. Calibration
cannot recover information which the hardware has clipped or failed to
observe, and the software must not claim that a flat trace contains bits.

The previous physical measurements used the deleted implementation. This
replacement has not been flashed or qualified on a real Stick S3. Its IRAM
report is explicitly not a physical timing certificate; release packaging
requires the existing prerelease override until hardware qualification exists.
Read-only capture now exports the retained 16-bit observations and discontinuity
metadata for that work.
