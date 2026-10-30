# Connection reliability

A transfer can fail after its opening handshake succeeds. HGSS sends hundreds of chunks during an initial upload and abandons an established exchange after an unrecovered packet error. Receiver errors therefore matter even when a device usually connects.

The receiver now fits packet length and bit timing together. An unlearned timing estimate cannot use the fast-pass shortcut. Learned timing can use the shortcut after protocol validation. The decoder requires valid UART start and stop bits, searches for a start after a bounded short noise prefix, and limits the fitting work. An isolated pulse one byte before a packet needs additional protocol validation. The receive buffer allows two extra bytes while the protocol checks the command, body size, checksum and session before removing that prefix. The native packet buffer remains 136 bytes.

Short rejected flashes leave the sampler listening. After a released event, listening resumes without the no-event scheduler delay or millisecond alignment wait. Late measurements use the reserved value 254, distinct from a valid delay and from no threshold crossing. A late measurement cannot establish an idle bit. A healthy pulse in the same cell can still establish a zero. The receiver learns timing only after protocol validation.

Packet validation rejects short headers, oversized reads, incomplete command bodies and H8 RAM-write requests. It preserves partial page tails, unaligned EEPROM writes and clock-bearing reset packets. The shared original-H8 code stays behind its existing target guards.

## Diode detection and the margin

The charge-safety check still prevents the sampler from driving a held-low input. A blocked input now has its own warning. Capacitance-based absence detection is advisory, so an uncertain absence estimate does not prevent valid reception.

The connection record keeps the original presence measurement, receiver-start safety, accepted packets and final result independently. Settings reads the same record used for the margin. A received packet supersedes an earlier advisory absence estimate in the margin while preserving both facts in Last connection. The result is finalized after native completion and storage health checks.

The solid diode indicates a detected component. Curved waves indicate accepted protocol traffic. The game reports transfer completion. Single-byte probes and raw receiver activity alone do not earn the waves. Successful operations clear the icon; failures can retain useful evidence while their result remains open. Unregistered devices return home after the native result animation, which also clears the margin. Unknown preflight is quiet while safe listening proceeds.

Actionable results recheck every two seconds while awake and visible, with two consistent safe results and at least one second between ordinary displayed changes. Unsafe input is handled immediately. A corrected diode is shown for two seconds. Checks pause in Settings and stop on sleep or result exit; opening Settings preserves history. A physical presence check previously took about 129 ms on one device; this is not a timing guarantee for other hardware. No additional probes or margin drawing run during active reception or storage replies.

Battery status uses the other end of the connector margin and follows rotation and appearance. Healthy play and ordinary wake have no battery icon. USB observations share the runtime's existing one-second power sampling; the display requires two consistent valid observations for a change. Unknown supply measurements do not become unplug events. Sleep decisions retain the existing conservative behavior on a failed supply read.

USB changes visible during ordinary play appear for 2.5 seconds from drawing, with a five-second freshness limit. Events during sleep, Settings or communication are resynchronized without a notification. Battery voltage is sampled every five seconds while awake outside communication. Two readings below 3.34 V establish low battery; two at or above 3.44 V clear it. These retain the existing low-entry threshold with hysteresis; they are not a calibrated runtime estimate. Transient failed reads retain the last estimate for up to 15 seconds, and do not clear an established low warning. Settings reports unavailable measurements. Confirmed USB power suppresses the steady low warning and does not imply charging.

## Timing and storage

The storage format, checkpoint layout, journal and commit ordering are unchanged. The firmware retains the complete request-to-reply time and the interval between completed replies. Those intervals include decoding, request transmission, storage and response transmission. A reply interval of at least 100 ms increments the deadline-miss counter.

The firmware skips optional LCD work before established protocol replies. It publishes the session result after the final storage-health check, so a failed completion write cannot produce a successful diagnostic result.

## Receiver margin

The charge-and-discharge receiver depends on the diode, contact, board characteristics, light and alignment. The current capture and decoder cutoff remains 124. Changing only the decoder threshold cannot recover light that never starts a capture, and a lower cutoff cannot repair saturation.

The physical check on 2026-10-29 detected the inserted diode and completed 69,120 passive gates without a transport fault. The final 32,768 retained gates were above the activity cutoff. Their most frequent value was 144. Two late gates occurred across the complete passive run. This checks one unit under its current room conditions. It does not measure the signal margin of another diode, board, cartridge or lighting setup.

Automatic sensitivity adjustment needs measurements of both background and actual transmitted light before it can replace the fixed cutoff. The raw-capture command retains a failed burst without changing saved data. Background and representative transmitted-light measurements still need separate hardware capture. A physical holder or shroud may help contact, aiming or unwanted light, but it needs separate comparison on hardware.

## Recovery with stock HGSS

Automatic operation restart remains disabled. A probe compiled unchanged functions from [the audited HGSS source](https://github.com/ctrl-cheeb-del/pokeheartgold/blob/2e0a4e000ff3c9ae5e8f41ad8a2e493eca49d87e/src/application/pokewalker/pokewalker_ir.c) and its status source with recording card and clock substitutes.

The probe confirmed that an active link ignores a one-byte FC probe. An eight-byte FC packet can trigger FA and F8, replace the session token and restart status negotiation. It also found two constraints that prevent treating this as a qualified recovery mechanism:

- Re-handshaking retains the game's total chunk count. Replaying a complete 368-chunk upload after ten earlier chunks produces a count of 378, and the game's eight-bit progress result wraps to 6.
- During return, receiving 0x26 makes HGSS send 0x4E, report completion and close. A Walker that has not received that commit cannot safely assume the game is still waiting to restart.

A 100 ms receive timeout also closes and resets the game link. Increasing the Walker timeout cannot make HGSS wait longer. Unsolicited completion replies and duplicate acknowledgements can advance the game, so they are unsuitable as generic keepalives.

Qualification of automatic restart requires a two-sided exchange using the stock game handlers, faults at each chunk and commit boundary, and comparisons of the resulting game and Walker data. A handshake transition alone does not establish safe recovery.

## Validation

`stick/check.py` runs the compiled production firmware against simulated GPIO, optical signals and storage. The connection scenarios include full uploads, compressed uploads, Walker exchange, readback, slower input timing, leading noise, a one-byte noise prefix, malformed packets and interrupted storage. The UI scenarios check diode states, both rotations, both appearances, input layouts and wake behavior.

CI builds the firmware and simulator once per platform, then runs the scenarios in five shards. A final check verifies that every scenario completed with matching build and test inputs. For a focused local run, pass `--scenario diode`; to run one group, pass `--shard 1/5`.

Each run retains its firmware image, source hashes, screenshots, optical traffic, saved flash and results in `stick/.build/`. Simulator results establish behavior under the simulated inputs. Physical cartridge transfers, other units and lighting conditions require hardware qualification.

The physical app-only upgrade check starts with a full flash backup and compares the decoded EEPROM before and after boot. Registration and Pokémon data must survive. The private backup and EEPROM exports stay outside version control.

[Receiver validation](../audits/reliability-2026-10-29.md) and [margin validation](../audits/margin-status-2026-10-29.md) record the measured results.
