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

Protocol behavior comes from the matching `pw` source, including its command
table, checksum, session transitions, and timeout decisions. Target-only
conversions reproduce H8 in-memory byte and bit layout at the wire or EEPROM
boundary. Bench traces are validation evidence, not rules for changing that
behavior. In particular, H8-native counters and the session token are
big-endian, while fields suffixed `Le` are forwarded in their existing bytes.

The receiver belongs in this target's hardware layer. Implement acquisition,
sampling, decoding, bounded byte delivery, half-duplex ownership, and error
reporting here against the native IR protocol code. The bench `.137` image is
the waveform and behavioral reference, not a source file to extract or include.
Its sampler is physically timing-sensitive, so compare the port's generated
instructions and requalify it with sealed traffic and complete exchanges.

## Current state

- The source base is the current `pw` decompilation.
- `controls.cpp` is a target-only input adapter for the two-button comfort
  profile and three-button profile. The physical M, R, and L switches were
  counted on the Stick; an M+R chord delivered native Center and opened the
  original menu. Its host test covers chord ordering, late second presses,
  short taps across native scans, and both landscape orientations. The chord
  window is selectable as 80, 120, or 160 ms.
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
- The input seam feeds native `InputScan` through debounced Stick button
  levels and now supplies the original Center IRQ wake latch. A forced
  power-save to ten-scan Center hold trial on-device returned to interactive
  mode. The NT7508 drawing code writes into a two-bank virtual panel whose
  96×64 view is scaled onto the Stick display; NT7508 power-save now also
  switches the physical backlight off. The virtual bus has a host test.
- The storage seam retains the native mirror/repair algorithm. It maps the
  64 KiB EEPROM image to two checked LittleFS slots. Small changes use a
  checked append journal; a mirrored source write is one journal transaction.
  Boot replays the journal into a new checked image slot, and IR defers flash
  commits until the session ends. Only a completely erased data partition may
  be formatted. The source mirror and SaveData wire-order tests run under
  sanitizers. A changed setting survived a hardware reset with both save
  mirrors and checksums valid; interrupted-write recovery still needs a board
  fault-injection test.
- Explicit target bitfield ordering keeps the original byte masks on the
  ESP32, and the native IR token and step counters are emitted in H8 byte
  order. These are focused compatibility changes, not a complete endian
  audit of every EEPROM and protocol record.
- Peer step counters are decoded from H8 byte order only when the application
  calculates a gift or builds a diary entry. Diary counters and saved daily
  history retain H8 byte order; the trainer history view decodes that stored
  value. A sanitizer-backed diary record test covers these boundaries.
- The target RMT transmitter, continuous optical task, native foreground
  scheduler, motion sampler, RTC events, display, storage, and sound adapters
  now compile together and boot on the Stick S3. The full image was flashed
  as an app-only update after preserving and hashing the prior 8 MiB flash.
- The application now keeps its native `MainTick` order while sampling the
  Stick's BMI270 at the native 62.5 ms cadence; the scaling follows the
  BMA150 high-byte convention. The target RTC generates the same quarter,
  minute, and hour event flags from a cooperative scheduler, and the scratch
  allocator retains full ESP32 pointers instead of truncating them to the
  H8's 16-bit address width. These target paths have syntax and focused
  host checks. The board boots and continues native `MainTick`; physical axis
  calibration remains open. The speaker produces game audio, and the user
  confirmed the shorter cues are now audible.
- A source-faithful 3DS entry completed all 368 pages with 374 valid Stick
  receives and no invalid packets in
  `stick/.build/trials/port-hgss-real-image-atomic-complete/`. Independent
  EEPROM readback matched the HGSS source-built image and the transmitted
  course byte for byte. The course and record in that trial were sealed random
  payloads, so it proves transport and storage rather than a coherent game
  screen. Full repeated transaction qualification remains open. A frozen `.137`
  app image remains at `../bench/experiments/g5-terminal-start-137/app.bin`;
  its full pre-port flash backup is also retained under `stick/.build/`.
- A source-faithful registered-walker `back` transaction on the integrated
  firmware received 90 valid bursts, ended with the original HGSS DONE result,
  and cleared the saved Pokémon flag while retaining registration. An `entry`
  request on that already registered walker was correctly rejected by HGSS.
  The bench artifacts are under `stick/.build/trials/journal-hgss-005-back/`.
- Two subsequent source-faithful `put` transactions and an intervening `back`
  completed on the same Stick boot (`journal-hgss-006-put` through
  `journal-hgss-008-put`). The two `put` runs each received 133 valid bursts
  without an invalid packet, and the intervening `back` received 90 valid
  bursts without an invalid packet. Each operation reached the original PHC
  DONE state. Independent 64 KiB EEPROM readback after both `put` runs matched
  every byte of the 10,430-byte course fixture, with both status copies and
  checksums valid and the Pokémon flag set. The fixture comes from HGSS's
  first-course table, compressed artwork, and course background. It chooses a
  plausible starter and renders English labels because no trainer save or DS
  message renderer is running on this bench; `hgss_course_fixture.py` records
  these choices. The 3DS still executes the original PHC protocol engine.
- A further same-boot diagnostic series completed ten `back`/`put` cycles:
  20/20 original PHC operations reached DONE, with 90/90 valid packets in
  each `back` and 133/133 in each `put`. The ledger and independent final
  EEPROM verification are in `stick/.build/trials/opening-repeat-ledger.json`
  and `opening-repeat-final-verification.json`. The first attempted repeat
  before this series failed on `put` page 59: the source 3DS transmitted wire
  `AA` in the page's first byte, the Stick decoded `AB`, the source checksum
  rejected it, no ACK was sent, and both ends timed out. That trial is retained
  as `journal-repeat-01-put`. The later 20/20 does not remove this failure.
  A bench-only 96-gate opening snapshot now prints when a checksum fails so
  the next occurrence can distinguish weak/missing optical observation from
  phase assignment. The flashed diagnostic app SHA-256 is
  `b8ec2c57cf16cf8834e9f1816adb95d2666ba6542ee4dad7ada4b724a9325dd9`.
- A longer frozen-image diagnostic recorded 44 successful whole operations
  before `cycle-023-back` failed. The source 3DS sent a page control packet
  beginning `A6`; the Stick decoded `A7`, the source checksum rejected it,
  and the peer timed out. Two release-late gate sentinels occur near that
  packet's opening UART cells. The artifact retains the full trace and the
  opening gate snapshot under `stick/.build/trials/opening-long-001/`.
  The sentinel encoding overwrites any measured crossing on those gates,
  so the capture does not distinguish a missed optical pulse from a late
  sample. All 45 attempts remain in the ledger.
- A power audit found the ESP32 remained active at 240 MHz with 16 Hz motion
  sampling even after the source entered inactive mode. The port now uses the
  source's one-second inactive sample cadence, temporarily returning to
  16 Hz for a physical or queued button gesture so wake controls remain
  prompt. It gates the external 5 V IR rail around sessions, disables the
  BMI270's unused gyro and temperature sensors, carries the last good
  acceleration sample across an I2C/data-ready miss, and reads voltage from
  the initialized PMIC instead of uninitialized `M5.Power`. On the flashed
  candidate, the external rail read off after `back` and `put`; the inactive
  test counted six main ticks in 5.2 seconds, and held Center woke the source
  game within 1.4 seconds. A 20-operation same-boot series on the first
  power candidate passed with exact course readback and valid status mirrors;
  the final candidate passed a complete `back` and `put` after the wake
  test, then 10/10 further whole operations on one boot with exact course
  readback and valid status mirrors. Research, assumptions, and measurement
  needs are in `stick/docs/power-source-research.md`.
- The screen-off foreground now enters ESP32-S3 Light-sleep between the
  original 16 Hz walking samples or one-second inactive samples. M and R are
  RTC GPIO wake sources; the PM1 side-key event is polled within 100 ms.
  State, clock deadlines, and the original motion estimator stay in RAM.
  The receiver, sound playback, menus, and active gestures keep their normal
  timing. USB power suppresses automatic sleep because the USB Serial/JTAG
  connection disconnects during Light-sleep. A bounded bench trial verified
  101 sleep cycles and about 4.70 seconds in the sleep call over five seconds
  at 16 Hz; an inactive trial verified 55 cycles, about 4.92 seconds in the
  sleep call, and seven successful BMI270 samples. A source-faithful `back`
  and `put` passed afterward, with 90/90 and 133/133 valid receives and exact
  EEPROM course readback. Battery current and actual runtime remain unmeasured.
- The current serial `c` trigger and verbose burst output are enabled only
  with `PW_STICK_BENCH_CONTROL`; they are bringup instruments, not protocol
  decision makers. The 3DS runs the source-owned retail peer. The native
  foreground holds 16 ticks/s, 4 UI frames/s, and one RTC second per wall
  second. The sound score reaches the Stick's I2S speaker and codec with no
  reported start or tone failure; the user confirmed short sounds improved.
  The user also confirmed responsive controls and the 20 ms M+R hold. A
  Settings save's measured main-loop gap fell from 3.48 seconds to 12 ms.

## Stick controls

The default Comfort layout in left-side-down orientation maps M to native
Left, R to native Right, M+R held together to native Center, and L to the
Stick settings screen. The direction mapping follows the observed movement
of the original menu's selected icon on this panel. The first press is held
for the selected 80/120/160 ms chord window; Center requires both buttons
to remain pressed together for 20 ms after the second debounced press. A
fleeting overlap while alternating directions produces direction taps.
Once a single
direction has been emitted, a late second press cannot become Center until
both buttons are released. Short taps are queued for the next native input
scan. A held Center wakes the original game after eight 62.5 ms scans.

In Stick settings, M moves to the next row, R changes that row, and L closes
the menu. Settings include Comfort or Three Button input, either landscape
orientation, an 80/120/160 ms chord window, and a one-second speaker test.
They persist separately from the Pokéwalker's 64 KiB EEPROM. In Three Button
mode, M is native Center, R and L are the directional keys; hold L for 1.2
seconds to open Stick settings. Device settings pause game input while keeping
the original clock and foreground code running.
