# Stick S3 port

This branch starts from the current `pw` decompilation at
`6dc7bc09950078fa3fe0dffa4dae34e9549a99da`. The source under `src/` and
`include/` is the Pokéwalker firmware reconstruction. The `stick/` directory
holds the target-specific adaptation. This branch can change the C source as
needed for the ESP32-S3; matching the H8 ROM is not a port requirement.

The earlier browser-based experiment is preserved outside this repository at
`../pw-sim-port-prototype`. It is an implementation reference only: its source
snapshot is older, and its browser connection backend uses a timeout stub.
Files from that experiment must not replace the current firmware sources.

The port must preserve the original application's state machine and packet
format while substituting the H8 peripherals with Stick drivers. In particular,
`src/support/ir.c` is the protocol authority. The Stick's optical receiver
hands it complete raw UART bursts with final-stop timestamps; the transmitter
accepts logical bytes and applies the original transport XOR. The UI,
accelerometer, EEPROM, RTC, and power services need
explicit hardware adapters. The H8's 16-bit `int` and pointers, packed record
layouts, endian behavior, and shared storage overlays need validation on the
ESP32-S3 before this can be called a firmware port.

The receiver belongs in this target's hardware layer. Implement acquisition,
sampling, decoding, bounded byte delivery, half-duplex ownership, and error
reporting here against the native IR protocol code. The bench `.137` image is
the waveform and behavioral reference, not a source file to extract or include.
Its sampler is physically timing-sensitive, so compare the port's generated
instructions and requalify it with sealed traffic and complete exchanges.

## Current state

- The source base is the current `pw` decompilation.
- `controls.cpp` is a target-only input adapter for the two-button comfort
  profile and three-button profile. Its host test covers chord ordering, short
  taps across native scans, and both landscape orientations.
- `src/support/ir.c` now has a guarded target seam for the physical transport;
  its packet and session logic remains the original source. That source passes
  a native syntax check with 2-byte struct packing and 16-bit `uint`.
- `ir_rx_core.cpp` is a port-owned, payload-blind UART decoder for bounded
  optical bursts. Synthetic tests cover all 256 one-byte values and random
  payloads through 136 bytes. A read-only replay of 50 saved `.135` optical
  captures matched the frozen decoded wire bytes on 49; the one difference
  was an already invalid, poorly framed first burst. Sixteen saved `.137`
  short captures matched 16/16; `.137` long-capture differences were likewise
  invalid first bursts. These are decoder checks, not Stick-port reception
  proof.
- `ir_gate.cpp` is the port's GPIO5 charge measurement. Its isolated Stick S3
  Arduino build succeeds. The 91 Xtensa instructions in `measure_ir_gate`
  match the frozen `.137` gate instruction bytes after accounting for link
  relocations; there are no calls in the gate. The surrounding sampler and
  firmware still need a separate waveform and live receive check.
- No Stick firmware from this branch has been built or flashed. The optical
  receiver, protocol bridge, native runtime, display, motion, persistence, and
  complete transaction still require integration and hardware validation.

The existing `.137` receiver qualification campaign uses a separate firmware
under `../bench/`. Do not flash this port over that bench image while the
qualification campaign is running.
