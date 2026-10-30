# Connection reliability

StickWalker keeps the Pokéwalker protocol and replaces the Stick-specific optical transport. There is no receiver calibration screen, training exchange, saved sensitivity, or learned baud rate. The same fixed receiver runs from the first packet of every session.

## Receive the signal continuously

During a receive turn, core 0 measures G5 at 345,600 gates per second into a 32,768-byte internal-SRAM ring. Acquisition does not wait for an absolute light threshold or a packet-shaped event. Its only responsibilities are the electrical measurement, publication of raw samples, and explicit loss reporting. The foreground drains that evidence independently.

The fixed detector compares each observation with the largest value in its nine-sample neighborhood. This removes a local background offset without learning a per-device setting. Four samples of lookahead cost about 12 microseconds. The current contrast floor is six counts. It is a candidate constant verified in the signal model, **not a measured qualification across boards, diodes, or lighting conditions**.

Each UART start pulse establishes a fresh timing origin for one byte. The receiver checks the stop cell, assembles at most 136 wire bytes, and measures the inter-packet gap from the completed byte. That also handles `FF`, whose only light pulse is its start. It does not search alternative packet lengths, fit a packet-wide clock, repair weak bits, or ask the checksum which interpretation to prefer.

An acquisition erasure (`254`), missing gate, framing violation, or overflow invalidates the affected frame. After damage, the receiver waits for a full possible byte plus the frame gap in observed darkness before accepting another start. It cannot turn the suffix of an oversized or interrupted frame into a fresh packet. A censored dark observation (`255`) remains distinct from an erasure.

This design cannot reconstruct missing optical information. In particular, a pulse close enough to a real start can create an alternative, perfectly framed UART sequence. Native validation rejects the tested corrupted sequences; it does not slide the data until a checksum matches. Signal-envelope tests retain these negative results alongside successful cases.

## Own the receive and service turns explicitly

An accepted native command that grants a service turn can park acquisition for a reply or flash operation. A timed-out opening probe also owns its next retry and its intervening jitter/display work. Rejected traffic and valid ignored activity, including `0x2E`, leave reception running. A following request can therefore begin immediately after the normal packet boundary without disappearing into a scheduler gap.

The time-critical sampler masks core 0 interrupts and the core 1 scheduler tick during acquisition. It pauses the dedicated OS tick counter (SYSTIMER counter 1), so FreeRTOS virtual tick time and task delays pause during this exclusive mode. The native clock, `esp_timer`, RTC time, and hardware watchdog clocks continue independently. Resuming a service turn therefore does not replay a growing backlog of missed OS ticks. This is an explicit contract for the quiesced firmware; adding background services that require wall-time FreeRTOS delays requires revisiting it.

Watchdog supervision follows the same ownership: progress must be observed in both producer and consumer, and ordinary tick/idle supervision is restored for service and session exit. There is no arbitrary maximum receive-turn duration. Native inactivity and explicit acquisition faults still end broken links.

A reply uses an RMT DMA buffer large enough for the complete maximum-size frame. It does not depend on an encoder refill interrupt partway through the waveform. The pinned driver starts an idle channel synchronously. A timestamp after the public copy-encoder callback, the return from transmit, exact waveform duration, and the completion interrupt provide a bounded physical end interval. The receiver discards its own transmission through that interval and retains later samples, even if foreground processing resumes late. An end interval wider than the allowed guard fails the link instead of silently discarding an uncertain amount of peer traffic.

## Preserve native protocol and durable data

Command dispatch, session negotiation, bulk transfers, and completion actions remain in the original firmware. Stick-specific admission checks validate the body fields a handler consumes, session token, and relevant transfer phase before permitting a write or reply. Original H8 code remains behind the existing target guards.

Accepted packets update native inactivity from their captured arrival time. Malformed packets, wrong-token traffic, and rejected handshake duplicates do not extend the session. A packet that arrived on time is considered before a foreground-time timeout decision.

The EEPROM image, checkpoint layout, journal, and commit ordering remain compatible with existing saves. Flash work runs in a service turn; verified durable writes precede their acknowledgments. Whole request-to-reply timing belongs to the transport, not a separate per-append storage timer. Diagnostics retain both request-to-completed-reply time and the interval between completed local replies. The simulator also checks the stock game's complete 100 ms receive budget; a local timing interval cannot establish when an arbitrary remote peer began its timer.

## Recovery with stock HGSS

The game's final return (`0x4E`) and gift (`0x66`) instructions are one-way completion messages. If the game reports success but that instruction never reaches the Walker, the Walker must retain its data. The firmware does not infer receipt, invent a commit acknowledgment, or automatically replay an operation. The [stock game handlers](https://github.com/ctrl-cheeb-del/pokeheartgold/blob/8cbb4a19ab781611ff1767035dcaea6b47baeded/src/application/pokewalker/pokewalker_ir.c) establish those completion boundaries and the 100 ms receive timeout.

The [previous source investigation](../audits/reliability-2026-10-29.md) also found that re-handshaking retains the game's chunk counter. A new handshake alone cannot establish safe recovery after partial progress. The replacement transport preserves these native limits.

## Diode indication and diagnostics

The electrical charge-safety check prevents driving a held-low input. Capacitance-based absence detection remains advisory. A detected diode does not prove optical margin, and a raw pulse does not count as an accepted protocol packet.

Last connection retains the preflight observation, receive safety, accepted packets, final result, framing/checksum failures, acquisition loss, and reply timing. The margin uses that same record. Optional display, sound, logging, and presence probes stay outside active acquisition. See [controls](controls.md) for the indicators and [troubleshooting](troubleshooting.md) for the read-only capture command.

Version 2 raw captures contain the retained tail of the most recent continuous acquisition epoch, with its gate indices, time origin, and local-transmit exclusion boundary. A service gap is never presented as continuous optical evidence. The tail may start partway through a frame; the raw data is evidence, not a promised complete packet.

## Validation and remaining qualification

`stick/audits/ir_receiver_workflow.py` feeds an independently generated optical integration model into compiled production receiver code. It sweeps all byte values, phase, transmitter clock, pulse width, baseline, gain, missing prehistory, byte spacing, erasures, overflow, and timestamp wrap. It also records ambiguous noise and weak-signal failures.

`stick/check.py` runs the actual production ESP32-S3 firmware against GPIO-level light, RMT/GDMA descriptors, watchdogs, and persistent flash. Scenarios cover initial and subsequent uploads, compressed pages, return, gifts, Walker exchange, lost final instructions, wrong-token traffic, immediate post-keepalive requests, and maintenance-held receive turns. Corruption scenarios require preserved data after reboot. Existing UI, power, sleep, input, and storage-recovery scenarios remain in the suite. CI runs the firmware suite in five Linux shards and checks a macOS build.

Each run retains exact source hashes, its firmware image, optical traffic, saved flash, and results under `stick/.build/`. These checks establish behavior under the modeled inputs. They do not establish analog signal margin, silicon instruction/cache timing, DMA bus contention, or real cartridge interoperability. The historical instruction baseline is deliberately unchanged; replacing the producer does not create physical timing qualification. Real BPV10NF/StickS3/cartridge trials across units and lighting remain required before calling this hardware-qualified.

The [fixed-IR rebuild audit](../audits/fixed-ir-2026-10-30.md) records provenance, deletions, verification, and the remaining hardware qualification. The [2026-10-29 receiver audit](../audits/reliability-2026-10-29.md) describes the previous implementation. Its successful cases do not qualify this replacement.
