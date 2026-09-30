# Transfer trials

These results precede the current candidate. Raw traces and independent EEPROM
readbacks remain under ignored `stick/.build/trials/`. The 3DS peer runs the
source-owned PHC engine; generated course fixtures do not reproduce a trainer's
save or localized message renderer. They qualify their tested transport and
storage paths, not every retail workflow.

## Recorded operations

- A source-faithful 3DS entry completed all 368 pages with 374 valid Stick
  receives and no invalid packets in
  `stick/.build/trials/port-hgss-real-image-atomic-complete/`. Independent
  EEPROM readback matched the HGSS source-built image and the transmitted
  course byte for byte. The course and record in that trial were sealed random
  payloads, so it proves transport and storage rather than a coherent game
  screen. Full repeated transaction qualification remains open. A frozen `.137`
  app image remains at `../bench/experiments/g5-terminal-start-137/app.bin`;
  its full pre-port flash backup is also retained under `stick/.build/`.
- A real HeartGold Game Card completed a walk-start transfer with the Stick.
  Its optical UART clock measured about 2.9855 gates/cell, outside the old
  3.0000-3.0080 fit range learned from the 3DS bench transmitter. A wider
  full fit recovered its packets but took 132-149 ms, exceeding both the
  game's 100 ms reply timeout and the port's roughly 98 ms inactivity check.
  The receiver now searches the full start-phase range around the current
  burst's or a previous credible long burst's baud estimate, and computes
  the expensive phase score only for competitive fits. This uses UART framing
  only; the original protocol still checks commands, tokens, and checksums.
  The successful real-card trace recorded 374/374 valid optical bursts,
  no rejected burst or checksum failure, and a normal completion. Its first
  32 retained burst diagnostics all used the fast fit; maximum recorded decode time
  was 30.9 ms and maximum recorded reply-start time was 31.7 ms. A 65,536-byte
  EEPROM readback after transfer had matching status and save mirrors and
  clear commit markers. After reboot, the received course and staged-course
  regions remained exact; only mirrored save counters changed. The validated
  bench app hash is `56b27c350da65bde2efcd855709cfcbb87519a31f482f08a6ff4f69482f9a7c2`.
  The cleaned bench app had the same sampler machine bytes; its hash is
  `6e38abbed86906bb4d98d2ec3ecbe954f159a366d006929bff53df1b707bbc35`.
  The same source was then compiled without `PW_STICK_BENCH_CONTROL` and
  flashed as an app-only update. The diagnostic-free image hash is
  `317943ba93b54810ea5be57fc936574f70bf403b63a1196458b4b174b59129d9`.
  Flash verification passed, and the user reported that a normal HeartGold
  connection on this production image worked. This second real-card success
  is user-observed; the production image has no per-burst serial trace.
  Repeated quantitative qualification on one frozen production image remains
  open.
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

## After initial power changes

The first power candidate completed 20 same-boot operations with exact course
readback. After wake corrections, a later candidate completed a fresh `back`
and `put`, then 10/10 further operations with valid status mirrors and exact
course readback. Artifacts are under `power-001-repeat/` and `power-004-repeat/`.
The initial Light-sleep diagnostic also completed a `back` and `put` with
90/90 and 133/133 valid receives and all 10,430 course bytes matching.

These runs qualify their own images. See the [validation summary](../validation-history.md)
and [release checklist](../release.md) for the remaining production trials.
