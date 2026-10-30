# Connection reliability

A transfer can fail after its opening handshake succeeds. HGSS sends hundreds of chunks during an initial upload and abandons an established exchange after an unrecovered packet error. Receiver errors therefore matter even when a device usually connects.

The receiver now fits packet length and bit timing together. An unlearned timing estimate must complete the full search; validated learned timing can use the fast pass. It requires valid UART start and stop bits, searches for a start after a bounded short noise prefix, and limits the fitting work. An isolated pulse one byte before a packet needs additional protocol validation. The receive buffer allows two extra bytes while the protocol checks the command, body size, checksum and session before removing that prefix. The native packet buffer remains 136 bytes.

Short rejected flashes leave the sampler listening. After a released event, listening resumes without the no-event scheduler delay or millisecond alignment wait. Late measurements use the reserved value 254, distinct from a valid delay and from no threshold crossing. A late measurement cannot establish an idle bit. A healthy pulse in the same cell can still establish a zero. The receiver learns timing only after protocol validation.

Packet validation rejects short headers, oversized reads, incomplete command bodies and H8 RAM-write requests. It preserves partial page tails, unaligned EEPROM writes and clock-bearing reset packets. The shared original-H8 code stays behind its existing target guards.

## Diode detection and the margin

The charge-safety check still prevents the sampler from driving a held-low input. A blocked input now has its own warning. Capacitance-based absence detection is advisory, so an uncertain absence estimate does not prevent valid reception.

The diode indicator distinguishes a presence measurement from received protocol traffic. Received traffic remains visible briefly after an exchange. A connection result can retain the relevant diode state until the user leaves it. Icons do not redraw during active reception or storage replies.

The battery indicator occupies the other end of the connector margin. Both indicators follow rotation and appearance, and leave the original Walker display area available for the game. Battery bars use the existing voltage estimate. The cable mark means USB power is present. It does not establish whether the battery is charging.

See [controls](controls.md) for the icon meanings.

## Timing and storage

The storage format, checkpoint layout, journal and commit ordering are unchanged. The firmware retains the complete request-to-reply time and the interval between completed replies. Those intervals include decoding, request transmission, storage and response transmission. A reply interval of at least 100 ms increments the deadline-miss counter.

The firmware skips optional LCD work before established protocol replies. It publishes the session result after the final storage-health check, so a failed completion write cannot produce a successful diagnostic result.

## Receiver margin

The charge/discharge receiver depends on the diode, contact, board characteristics, light and alignment. The current capture and decoder cutoff remains 124. Changing only the decoder threshold cannot recover light that never starts a capture, and a lower cutoff cannot repair saturation.

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

Each run retains its firmware image, source hashes, screenshots, optical traffic, saved flash and results in `stick/.build/`. Simulator results establish behavior under the simulated inputs. Physical cartridge transfers, other units and lighting conditions require hardware qualification.

The physical app-only upgrade check starts with a full flash backup and compares the decoded EEPROM before and after boot. Registration and Pokémon data must survive. The private backup and EEPROM exports stay outside version control.

[Validation record](../audits/reliability-2026-10-29.md).
