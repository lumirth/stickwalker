# IR readiness and DS reply regression

PR #5 moved receive handoff and durable EEPROM work into the compiled Stick
port. The user subsequently reported both immediate DS connection failures and
failures after the transfer sound began. Hardware was unavailable for this
investigation.

The references are the matching [Pokéwalker firmware](https://github.com/lumirth/pw/tree/6dc7bc09950078fa3fe0dffa4dae34e9549a99da)
and the user's HGSS `src/phc/irc.c`, `phc_lib.c` and PHC record definitions.
The pre-PR comparison is `b3ef7a2`; its existing Watts byte-order defect was
accepted only by the diagnostic comparison peer, so it could exercise the
same acquisition and transfer sequence.

## Reproduction

The original ideal, short status/read exchange passed. A longer synthetic
uncompressed course transfer at the captured retail-card baud, with local
transmitter echo and separate CPU instruction-cost scheduling, failed on the
PR firmware. The pre-PR firmware completed the same exchange. One retained
failure changed a data bit in a 136-byte page, causing the native checksum to
reject it and the DS peer to reach its unchanged 100 ms timeout.

The course-only comparisons used the same 10,430-byte public payload
(`b5dcc0c82d28b8f9270aa4add67523011952a0c5aeb63253126188ee5574afa8`),
115760 baud, 5 ms peer turnaround and live local transmitter echo:

| Firmware | Result |
| --- | --- |
| Pre-PR `b3ef7a2` | All 82 course pages complete; legacy Watts accepted only for comparison |
| PR's original IR implementation at `68ae1fe` | Corrupted page; timeout after eight native transmissions |
| Sampling/handoff repair alone | Valid pages, then a late reply after 27 native transmissions |
| Sampling, bounded readback and exact decoder optimization | All 82 course pages complete |

The producer had acquired additional request/ack fences and handoff bookkeeping
inside its timing-sensitive gate function. The worker now stops at a complete
burst and performs notification and release waiting outside that function.
Readiness observes fresh progress in the existing published sample counter;
it still waits for physical samples before initial transmission and replies.
Receive-to-reply storage remains protected by a consumed-event ticket.

After removing the acquisition failure, the broader payload exposed late
replies during journal growth. The write path retains checked open, size, write,
sync and close operations. Its independent readback uses a fresh POSIX
descriptor to read only the appended header and payload, avoiding buffered
seek/read prefix work. Reader closure is checked too.

The decoder's complete candidate set, framing-first/phase-second ranking,
original tie order, baud range, phase range, strong/weak thresholds and recovery
rules are retained. It visits likely baud candidates first and rejects a
candidate only when its already-observed framing errors and nonnegative phase
sum prove it cannot win. Winning data-cell occupancy is built once. A
sanitizer-backed comparison against the original decoder matched all output
bytes, geometry, cached-baud behavior and recovery diagnostics on 2,048
generated waveforms. This creates reply-time headroom without accepting a
packet the previous search would reject.

## Permanent coverage

`peer-transfer` follows HGSS's uncompressed put choreography with public data:
status/Watts, 82 course pages, five trainer-record pages, PHC and distribution
reads, the checksum/ping exchange, commit and one-way shutdown. It checks the
native committed course and record independently after reset.

`peer-play` uses a synthetic eligible Walker and exchanges status, animation,
name, trainer-house records, peer information and start. It checks both bulk
transfer directions and the received resources and native peer-history entry
after reset. This exercises one handshake role, not all collision/backoff cases.

Run the exact production build through:

```sh
uv run python stick/check.py --scenario peer-transfer --scenario peer-play
```

These workflows execute the ROM, bootloader, FreeRTOS, SDK and application and
use optical/button/bus inputs. They do not inject native game state. Their
instruction-cost mode schedules the CPUs independently in one-instruction
quanta using the simulator's opcode, branch, call and load-use costs. It is a
software timing regression test; analog acquisition, cache contention, flash
part latency and retail hardware interoperability remain outside its model.
The frozen machine-byte timing reference remains a separate diagnostic.

## Verified candidate

All 13 production workflows pass on application
`05ed911a060fca5c3e023a7994e351ed553974e68789a5caff5e6283f31730e5`.
The full put completes 135 native transmissions, then independently verified
course/record persistence after reset. Its maximum modeled DS send-to-complete-
response interval is 91.99 ms against the unchanged 100 ms deadline. The same
exchange also completes on the fresh bench build. Walker resources and native
peer history survive reset.

Both builds' input and output hashes match their manifests. Static checks pass,
and Renesas 6.02.01 and 6.02.02 each reproduce the canonical 49,152-byte image
(`f9e210a3b74afbbd12c5a66a51cc05cb9fbac986805ff0a3bfb4be6074d15607`).
The frozen machine-byte comparison differs for all three tracked functions;
its reference has not been updated. These new images were not flashed.
