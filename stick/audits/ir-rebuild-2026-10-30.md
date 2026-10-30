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
five replacements contain 1,348 lines, a reduction of 144 lines (9.7%), before
counting the deleted replay wrapper and obsolete timing baseline. Test-model
extensions and evidence documentation are separate from this production-code
comparison.

The replacement removes these assumptions:

| Previous assumption | Replacement contract |
| --- | --- |
| An absolute crossing-delay cutoff identifies optical activity. | Every observation is retained while listening; local contrast identifies candidate pulses. |
| A crossing delay can be treated as an exact scalar. | Finite last-high/first-low intervals, censored lower bounds and invalid measurements have distinct representations. |
| Optical burst completion grants permission to stop sampling. | Only native admission of a handled request, a timeout, or termination grants a service turn. |
| A packet can supply a fitted baud and timing origin. | Each UART byte supplies its own start origin on one fixed sampling schedule. |
| A false UART start must eventually fail stop-bit framing. | Every observed start remains independently representable until native packet admission resolves competing interpretations. |
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
at offsets 65 and 560; threshold polling stops against the next origin minus
70 cycles. Each probe checks that cutoff before reading GPIO. The turn's
independent 140 ms wall deadline is checked inside each integration interval.

Each 16-bit observation contains two seven-bit bounds in two-cycle units, plus
explicit flags for censoring and invalidity. Finite measurements retain the
last-confirmed-high lower bound rounded down and first-confirmed-low upper
bound rounded up. Censored measurements retain only the lower bound. Valid
readout ends before the next gate, so its elapsed delay is below 135 cycles and
fits the 254-cycle encoding limit. The first observation after a service
interval marks a discontinuity. A late electrical phase is an erasure; it
cannot silently become a plausible dark sample.

`ir_receiver.cpp` takes the largest actual lower bound in a nine-observation
neighborhood, with four gates of lookahead and a fixed six-cycle contrast floor.
A pulse requires that reference to exceed the current finite upper bound by
the floor. Censored background can provide a conservative reference, but a
censored current sample cannot prove a pulse. This is a local signal operation,
not calibration state. The decoder needs no idle history and never uses
command expectations or checksums to choose bits.

For a constant dark crossing time `T`, every neighbor lower bound satisfies
`L <= T` and the current finite upper bound satisfies `T <= U`. Therefore
`max(L) - U <= 0`: probe quantization alone cannot create a light decision.
The earlier scalar representation lacked this property. Its ordinary compiled
finite interval was 17 cycles wide, larger than the six-cycle contrast floor.
With fixed resistance 24,545.3 ohms, capacitance 12 pF, threshold 1.6 V and
background current 0.05 microamps, a no-light run produced 141 scalar readings
of 54 cycles and 115 of 64 cycles in 256 settled observations. The continuous
physical RC model generated that variation without injecting receiver samples.
The replacement represents those readings as overlapping intervals `[36,54]`
and `[46,64]`. The earlier low- and high-resistance cases happened to quantize
identically and did not exercise this boundary. Preserving both measured bounds
removes the assumption without a learned reference, a tuned timing correction
or additional ring RAM. The complete-transfer suite now includes this fixed
24,545.3-ohm boundary profile, with readback and reboot verification.

### Common optical margin

The fixed schedule allocates time to photocurrent integration before measuring
the remaining crossing delay. It does not require a complete finite dark
measurement: a censored dark lower bound can supply the contrast reference.
This permits a longer exposure without extending the gate period or teardown.

In the pinned instruction-cost model, the actual GPIO release occurs between
origin plus 70 and 75 cycles, and the initial input read between plus 563 and
567. Every valid gate therefore has at least 488 cycles of floating integration;
the maximum gap between these integration windows is 207 cycles. A SIR pulse of
the specified widths can overlap at most two windows. One overlap must be at
least half the pulse duration minus that gap, regardless of the pulse's phase.
For a minimum pulse duration `P`, minimum integration `W`, maximum gate period
`G` (all in CPU cycles), and CPU frequency `f`, the sufficient charge condition is
`I_min * (P - (G - W)) / (2 * f) > C_max * (V_charge_max - V_threshold_min)`.
The pulse must be longer than the integration gap. This makes the electrical
envelope explicit; the dark-reference and valid-timing bounds are also required.

| Physical input family | Minimum pulse | Guaranteed overlap in one gate | Captured signal charge | Charge needed to reach the input threshold |
| --- | --- | --- | --- | --- |
| 22–65 kΩ, 12 pF, 1.6 V threshold, 60 µA signal | 388 cycles | 90.5 cycles | 22.625 pC | 20.4 pC |
| 45 kΩ, 11 pF, 1.5 V threshold, 80 µA signal | 339 cycles | 66 cycles | 22 pC | 19.8 pC |

The calculation uses a 3.3 V initial charge and conservatively ignores light
during pull-down and the helpful background discharge. For these stated circuit
inputs, at least one observation of each pulse is already low, giving a finite
upper bound of zero. This is an analytic bound over pulse phase, with about
11% charge headroom, rather than a fitted setting for one failing waveform.

The ramp timestamp is at most origin plus 579, leaving at least 45 cycles
before the fixed read cutoff. The confirmed-high probe at relative timestamp
37 supplies an outward-rounded lower bound of 36 cycles. Even the fastest
22 kΩ dark crossing occurs after 45.67 cycles from pull enable in this model,
so that probe remains high. Finite or censored dark observations therefore
provide at least 36 cycles, leaving 30 cycles beyond the six-cycle contrast floor
against the guaranteed zero upper bound of a lit observation.

The nine-gate neighborhood also contains a dark reference for ordinary SIR
data. Conservatively, one pulse can affect at most two observations: the
557-cycle physical observation aperture plus a pulse of at most 410 cycles
spans less than two gate periods. At pulse spacing of at least 2,000 cycles,
at most four pulses can affect any nine-gate window. At most eight observations
are affected, leaving one dark reference. Arbitrary dense noise does not have
this guarantee and must pass the separate framing and packet-admission rules.

These bounds apply to the stated circuit and instruction models. They do not
replace qualification of real GPIO latency, recharge behavior, diode
capacitance, photocurrent, background variation or physical timing interference.

### Byte and packet framing

UART framing uses three gates per bit. Each observed pulse is a possible byte
origin; its fixed interpretation has one deterministic successor. Origins that
converge share decoded tails in a bounded graph. Each possible packet follows
at most 136 bytes, with original wrapping Timer W timestamps deciding native
frame boundaries. Native validation selects among these fixed interpretations;
it cannot alter optical classifications, bit timing or byte values.

The graph stores 2,048 pulse origins. One unresolved 136-byte path contains at
most 1,224 pulses, with at most 16 pending byte origins and 512 additional pulses
from a 1,024-observation input batch: 1,752 entries fit within the fixed capacity.
A selected frame and its origin identifiers are copied, so examined prefixes
can be released while other interpretations remain. A work-limited poll must
finish ready graph work before admitting another raw batch. It returns to the
native timeout check between work allowances; overflow is an explicit failure.
An erased measurement or a light run wider than two gates discards all
uncommitted graph state in constant time, including a selected packet whose
competing interpretations remain unresolved. A queued pre-disruption frame
can be lost; missing evidence cannot turn it into a uniquely valid packet.

## Native ownership, storage and transmission

`ir_packet.h` describes whether a message is rejected, leaves reception open,
or invokes a native handler. Command shape, session state and checksum are
independent of optical decoding. A valid complete packet takes precedence over
its exact byte-boundary suffixes, including packet-shaped payload bytes. A
checked packet also dominates an overlapping unchecksummed FC probe. Distinct
checked interpretations of overlapping input are rejected as ambiguous. The
validation callback has no session, diagnostic-counter or storage side effects;
only the resolved frame reaches a native handler. The old special case for
leading `FF` bytes and the separate byte-suffix repair pass are gone.

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

The full noise scenario exposed a deeper framing error in an intermediate
replacement image. A pulse 26 microseconds before the Watts request created
the entirely valid UART interpretation `99e9590e637ece46ce8a02`, while the same
unchanged optical samples also supported the correct `0c02540e637ece46ce8a02`.
The interpretations converged at their fourth byte; there was no later UART
contradiction to trigger recovery. Acquisition had no erasures. The greedy
receiver was therefore replaced with the origin graph described above, rather
than adding a command whitelist, timing search or error-triggered rewind. The
other 36 scenarios passed on that intermediate image; it was not accepted as a
complete verification pass.

The first interval-based candidate then passed all 38 local workflows, but its
[CI build failed the RC boundary transfer](https://github.com/lumirth/stickwalker/actions/runs/41115014249)
after 35 transmitted replies. The retained CI image and simulator reproduced
the same failure locally, with byte-identical peer report and SRAM capture.
The sampler instruction bytes matched the local build, but the two builds
entered the exchange at different phases. There were no acquisition erasures,
overwide pulses, ambiguity rejections or capacity faults. Of the 136 request
bytes, the first decoded as `00` instead of `02`; the other 135 were exact.

The missed bit split across two gates, each measured as `[16,34]`, while the
nearby dark lower bounds were 36. Only two cycles of contrast were proven, below
the fixed six-cycle requirement. The bounds were honest; the earlier schedule
did not guarantee enough exposure when a pulse straddled the recharge interval.
The replacement moves the fixed readout from offset 450 to 560, giving the
common optical margin derived above. The classifier, contrast floor, local
window and readout cutoff retain their contracts. This correction changes the
acquisition schedule rather than accepting an unproven pulse.

An additional complete-transfer case uses the existing 45 kΩ / 11 pF / 1.5 V /
80 µA circuit and 339-cycle pulse, at 115,200 baud with seven extra cycles
between bytes. Each byte spans exactly 30 nominal gates before that gap, so
successive byte origins deliberately move through the sampling phase. The
136 starts in a maximum frame cover the phase circle with gaps no larger than
about 7.7 cycles. The fixture drives ordinary light pulses and verifies the full
transfer, native commit and reboot recovery. It expands phase coverage without
using acquisition state to select the stimulus or expected bytes.

The malformed-input workflow also includes a deliberately ambiguous physical
pulse train. Its 112-byte page-write interpretation begins `02b79e43`, while
an earlier UART origin produces `a007eee0`; both have valid checksums, the same
session token and the same 104-byte body. Three ordinary SIR pulses precede the
page by five, four and two bit periods. The interpretations merge before the
token. Neither may trigger a handler or EEPROM write. The independent peer
then requires an unambiguous read of the target page to return its unchanged
bytes and complete normally.

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
| Build ID | `c0a1d71d25c5` |
| Application SHA256 | `35d26a5395b9bfb6a83fa79a0df8edd738ff9c691c3fab69d5e09e6abdf125df` |
| ELF SHA256 | `1580b52a671200eb1047f009d091f9795450056455e03fd81354bb390a2b3f6b` |
| Acquisition function SHA256 | `5ae94b8d5739085ccbd71bf568d8184e3352169878657f55f459b40589bcd805` |

The acquisition function occupies 579 bytes entirely in IRAM, with no direct or
indirect calls and no new stack spills. Its body is identical in the production
and bench builds. The compiled dark-readout path has at least 30 cycles before
the next gate in the instruction-cost model, and at least 97 cycles of recharge
before the following release. Finite readout leaves at least 39 cycles before
the next gate. The first observation of every receive turn is explicitly invalid
and cannot become peer data. This does not assert zero timing error on hardware.

The full verification workflow contains 39 scenarios. Its final completion
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
