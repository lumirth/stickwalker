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

## Initial software candidate (`22769ce`)

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

## Retail put failure and flash busy timing

The initial local-artwork candidate was installed and app/non-app flash verified.
The user then reported successful return from stroll, failed start of stroll, and
occasional black screens. A single subsequent diagnostic attempt decoded all five
incoming frames with no checksum or malformed-page rejection. They were response,
status, put request, and compressed course pages at `0xD700` and `0xD780`.
The last acknowledgment began 108.039 ms after the peer's final pulse; the journal
append maximum was 98.022 ms. The earlier production capture retained a 103.471 ms
append. No crash was captured; the user could not reproduce that symptom.

HGSS `compress`/`sendData` independently compress 128-byte course and trainer pages
using Nintendo LZ10, falling back to raw pages when compression does not help.
The last partial page is sent in a padded 128-byte form. The original full-put
workflow covered only uncompressed writes and therefore omitted this path.

The SPI flash model also completed programming and erases instantly. Optional WIP
busy periods now exercise the real SDK's polling: 300 microseconds per program
command and 20 milliseconds per sector erase are explicit stress parameters, not
claimed measurements of the installed flash chip. With those periods and the
compressed put sequence, the installed filesystem-backed implementation fails
unattended: a 116.078 ms append produces a 132.544 ms acknowledgment and the peer's
unchanged 100 ms timeout.

The backend now uses a single flash-record format in the existing game-data
partition. Two checkpoints cover a bounded log. Headers and bodies are programmed
and independently read back before a final publication word. Log space is erased
before acquisition; append operations cannot erase during an IR session. Checkpoint
replacement and journal clearing run after publication outside that session.
The filesystem and old save/configuration conversions are removed.

The first raw-log build completes the full uncompressed put and Walker exchange
with the same flash-busy profile; its maximum modeled DS response is 39.04 ms.
Recovery checks cover current records, stale records and an unpublished truncated
append. Permanent compressed-put coverage and a bus-observed zero-erase invariant
protect the timing window. Unsupported filesystem data and a corrupt published
checkpoint are rejected without conversion or erasure.

This storage format requires initializing the game-data region. Return the Pokémon
to HGSS before pre-release updates; see the [install guide](../install.md).

The final public and local-artwork production images each pass all 14 workflows,
including compressed put and the zero-erase reply-window check. Public application
SHA-256: `a8aa2659834cfed7deeb800a95bca5a57ff360ee415897d78377dd4aaa01591b`.
Local application SHA-256: `a5b890cad28ec7f800d98944243e12cb35c987aad983046717734faff57c7ab4`.
Independent negative boots also reject unsupported data, a corrupt published
checkpoint, and a corrupt newer checkpoint even when an older valid save exists;
all leave the data partition unchanged. Source/output hashes and static checks pass.
