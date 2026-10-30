# Connection reliability

A transfer can fail after its opening handshake succeeds. HGSS sends hundreds of chunks during an initial upload and abandons an established exchange after an unrecovered packet error. Receiver errors therefore matter even when a device usually connects.

## Link architecture

The GPIO acquisition engine, streaming UART receiver and transmit owner replace the old optical-event transport. There is no calibration pass, learned baud estimate, absolute activity cutoff, packet-wide timing search, or alternate legacy receiver. Every physical observation enters a fixed internal-RAM ring while the native session is listening. Rejected light and malformed packets do not stop acquisition.

An observation records a finite interval between the last confirmed high and first confirmed low input, a lower bound when the pin remained high, or an explicit invalid measurement. Both finite bounds fit in the existing 16-bit observation, rounded outward in two-cycle units. The largest lower bound within nine gates supplies the contrast reference; only a finite upper bound below that reference by the fixed margin proves a pulse. A constant dark delay cannot create contrast solely through polling quantization. Four gates of lookahead are ordinary decoding latency; they do not require an idle training period. Every observed pulse is a possible UART start. Each origin has one fixed three-gate bit interpretation and a deterministic successor; interpretations which converge share the same decoded tail. Neither checksums nor expected commands change optical classifications, bit timing or byte values.

The receiver follows each possible origin through at most 136 bytes and uses a pure native command/body, session and checksum check to resolve packet framing. A checked packet dominates its exact byte-boundary suffixes, so packet-shaped payload bytes cannot override a valid enclosing packet. A checked packet also takes precedence over an overlapping unchecksummed single-byte probe. Distinct checked interpretations of overlapping input are rejected as ambiguous. Only a resolved packet reaches a native handler. This replaces both the special case for leading `FF` bytes and the assumption that a false start must eventually cause a UART error. Packet admission also rejects invalid compressed pages, oversized EEPROM accesses and H8 RAM-write requests before granting a service turn.

The graph holds 2,048 pulse origins. Each native poll performs bounded graph work and consumes at most 1,024 new observations. When its work allowance expires, it returns to the native timeout check and resumes that work before admitting more observations. Selected bytes and their origin identifiers are copied, allowing examined graph prefixes to be released even while competing interpretations remain. Capacity exhaustion is an explicit failure; it never discards a competing interpretation and silently accepts another. An erased measurement or a pulse wider than the fixed two-gate limit discards all uncommitted interpretations in constant time. This can lose a queued frame preceding the disruption, but prevents missing evidence from manufacturing a unique interpretation.

## Diode detection and the margin

The charge-safety check still prevents the sampler from driving a held-low input. A blocked input now has its own warning. Capacitance-based absence detection is advisory, so an uncertain absence estimate does not prevent valid reception.

The connection record keeps the original presence measurement, receiver-start safety, accepted packets and final result independently. Settings reads the same record used for the margin. A received packet supersedes an earlier advisory absence estimate in the margin while preserving both facts in Last connection. The result is finalized after native completion and storage health checks.

The solid diode indicates a detected component. Curved waves indicate accepted protocol traffic. The game reports transfer completion. Single-byte probes and raw receiver activity alone do not earn the waves. Successful operations clear the icon; failures can retain useful evidence while their result remains open. Unregistered devices return home after the native result animation, which also clears the margin. Unknown preflight is quiet while safe listening proceeds.

Actionable results recheck every two seconds while awake and visible, with two consistent safe results and at least one second between ordinary displayed changes. Unsafe input is handled immediately. A corrected diode is shown for two seconds. Checks pause in Settings and stop on sleep or result exit; opening Settings preserves history. A physical presence check previously took about 129 ms on one device; this is not a timing guarantee for other hardware. No additional probes or margin drawing run during active reception or storage replies.

Battery status uses the other end of the connector margin and follows rotation and appearance. Healthy play and ordinary wake have no battery icon. USB observations share the runtime's existing one-second power sampling; the display requires two consistent valid observations for a change. Unknown supply measurements do not become unplug events. Sleep decisions retain the existing conservative behavior on a failed supply read.

USB changes visible during ordinary play appear for 2.5 seconds from drawing, with a five-second freshness limit. Events during sleep, Settings or communication are resynchronized without a notification. Battery voltage is sampled every five seconds while awake outside communication. Two readings below 3.34 V establish low battery; two at or above 3.44 V clear it. These retain the existing low-entry threshold with hysteresis; they are not a calibrated runtime estimate. Transient failed reads retain the last estimate for up to 15 seconds, and do not clear an established low warning. Settings reports unavailable measurements. Confirmed USB power suppresses the steady low warning and does not imply charging.

## Timing and storage

The native protocol owns receive and service turns. A validated request which its handler will answer, store or finish grants a service turn. The producer parks and restores interrupts before flash work. Journal appends enforce that ownership. A page-write reply is launched only after its storage has completed and the fresh receive stream is already running. The EEPROM image, checkpoint layout, journal format and durable-before-page-acknowledgment ordering remain unchanged. Native operation-completion actions retain their existing ordering.

Transmit uses one complete RMT DMA transaction, with space for all 1,360 symbols of a maximum frame and its terminator. The sampler observes the hardware's sticky transmit-complete event before the driver clears it. It retains the final dark stop-bit observations and all subsequent input, so an immediate reply does not become part of a discarded self-echo burst. ISR or task completion time is not used as the optical boundary.

Frame separation follows the wrapping 32,768 Hz native Timer W clock and its phase, using original observation timestamps. A normal receive turn expires after 3,200 ticks (about 97.7 ms). An in-progress frame may finish within a fixed additional 685 ticks, derived from the maximum 136-byte frame and the native byte-gap rule. Noise and ignored messages, including `0x2E`, do not move that absolute deadline. This deliberately bounds unsupported peer-only streams instead of preserving the original UART loop's ability to be extended indefinitely by received bytes.

The acquisition task masks its interrupts and the other core's OS tick only for a bounded receive turn. Its independent 140 ms wall deadline includes the longest local transmission, the native deadline and frame-completion allowance. The deadline is checked once per gate, so expiry is observed at the next check; checks are about 2.9 µs apart at the nominal cadence. The pinned runtime's interrupt watchdog is 300 ms. Every service or timeout boundary restores interrupts; there are no periodic blind listening gaps or 30-second masked segments. Optional display, audio and power work stays outside reception.

The connection record measures accepted request-to-reply time and intervals between completed replies. An interval of at least 100 ms is recorded as a deadline miss. A single flash append taking less than 97.7 ms is no longer treated as proof that the entire exchange fits the peer's deadline. Final results remain dependent on storage health after native completion.

## Receiver margin and qualification

The decoder has one fixed relative contrast floor, currently six CPU cycles. The gate schedule and this floor are development candidates, not per-unit settings. The measured intervals account for polling uncertainty, including changes in the compiled probe loop; the detector never substitutes a finite upper bound for a reference lower bound. Qualification must still establish a common bound on physical background variation and a minimum observable pulse across the supported electrical, optical and timing envelope. In interval terms, the smallest guaranteed difference between a background lower bound and a lit upper bound must exceed the floor, while background variation alone must remain below it. Censored background observations can provide a conservative reference; an entirely flat or clipped signal cannot supply missing bits.

The old physical check on 2026-10-29 measured the previous acquisition code on one unit. It does not qualify this replacement. `check_timing.py` now records the actual acquisition instruction hashes and verifies IRAM residency and call targets; matching an old implementation's bytes is no longer the criterion. Passing that inspection is not a physical timing guarantee, and release packaging records this distinction.

The read-only capture command exports the retained 16-bit observations, invalid/censored flags, gate indices and the latest acquisition epoch's time origin. It can expose acquisition failures without modifying the saved Walker. Real cartridge transfers and variation across boards, diodes, light, alignment, battery conditions and interrupt interference remain the physical qualification work.

## Recovery with stock HGSS

Automatic operation restart remains disabled. A probe compiled unchanged functions from [the audited HGSS source](https://github.com/ctrl-cheeb-del/pokeheartgold/blob/2e0a4e000ff3c9ae5e8f41ad8a2e493eca49d87e/src/application/pokewalker/pokewalker_ir.c) and its status source with recording card and clock substitutes.

The probe confirmed that an active link ignores a one-byte FC probe. An eight-byte FC packet can trigger FA and F8, replace the session token and restart status negotiation. It also found two constraints that prevent treating this as a qualified recovery mechanism:

- Re-handshaking retains the game's total chunk count. Replaying a complete 368-chunk upload after ten earlier chunks produces a count of 378, and the game's eight-bit progress result wraps to 6.
- During return, receiving 0x26 makes HGSS send 0x4E, report completion and close. A Walker that has not received that commit cannot safely assume the game is still waiting to restart.

A 100 ms receive timeout also closes and resets the game link. Increasing the Walker timeout cannot make HGSS wait longer. Unsolicited completion replies and duplicate acknowledgements can advance the game, so they are unsuitable as generic keepalives.

Qualification of automatic restart requires a two-sided exchange using the stock game handlers, faults at each chunk and commit boundary, and comparisons of the resulting game and Walker data. A handshake transition alone does not establish safe recovery.

## Validation

`stick/check.py` runs the compiled production firmware against simulated GPIO, optical signals and storage. The connection scenarios include full uploads, compressed uploads, Walker exchange, readback, slower input timing, leading noise, a one-byte noise prefix, malformed packets and interrupted storage. Additional cases use an independent finite RC circuit, varied pulse width and inter-byte gaps, and an immediate peer reply with local optical echo. The [simulation model](../simulation/README.md) documents its inputs and limits. The UI scenarios check diode states, both rotations, both appearances, input layouts and wake behavior.

CI runs the full firmware suite in five Linux shards and checks the macOS build with settings and optical-storage scenarios. A final check verifies that every scenario completed with matching build and test inputs. For a focused local run, pass `--scenario diode`; to run one group, pass `--shard 1/5`.

Each run retains its firmware image, source hashes, screenshots, optical traffic, saved flash and results in `stick/.build/`. Simulator results establish behavior under the simulated inputs. Physical cartridge transfers, other units and lighting conditions require hardware qualification.

The physical app-only upgrade check starts with a full flash backup and compares the decoded EEPROM before and after boot. Registration and Pokémon data must survive. The private backup and EEPROM exports stay outside version control.

[The previous receiver audit](../audits/reliability-2026-10-29.md), [margin validation](../audits/margin-status-2026-10-29.md) and [workflow performance](../audits/ci-performance-2026-10-30.md) record the measured results.
