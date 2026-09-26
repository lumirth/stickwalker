# Short cue scheduling — 2026-09-26

## Reproduced failure

The review correctly identified a wall-clock catch-up error. `MainTick()` hands
an EEPROM score to `BeepTick()` and enables its timer before the adapter
presents the image. The next loop services overdue piezo periods. With a warm
amplifier, no startup delay protects the newly emitted note from that backlog.
The original `BeepAdvance()` can start and mute a short note in one service
call, before the asynchronous speaker task adopts its request.

The reproduction compiles the actual `pw_buzzer.c`, resident pitch resources,
and `sound_bridge.cpp`. Scores come from the authoritative HGSS
`phc_sounddata.c` through the existing `sound_bytes()` extractor. Hardware is
stubbed, but the score walker, duration arithmetic and adapter calls are real.

    python3 stick/audits/sound_score_trace.py --output stick/.build/sound-scheduling-2026-09-26/final.json

The first regression run failed 102 of 176 timing cases. For a warm move cue
after an 80 ms stall, its only tone had duration **0 µs**. An 80 ms stalled
back cue likewise started and stopped its first two pitches at the same
timestamp. The before trace and logs are retained. Even an 8 ms stall clipped
the first note. These are command-timing observations, not microphone results.

## Correction

Native period writes reset Timer W. The adapter now anchors a newly emitted
note or silent period to the current output time. The service loop can catch
up the countdown of a note already playing, but cannot charge a newly started
note for cycles missed before it existed. A future cold-amplifier startup
deadline is retained. This uses the original score walker and note counts;
there is no minimum-duration replacement click or change to native controls.

A related issue was visible in the installed M5Unified implementation:
`_play_raw()` with channel `-1` chooses a free channel. `stop_current_sound`
preempts that selected channel; it does not replace tones on other channels.
Successive pitch changes could therefore accumulate sustained voices. Native
playback now explicitly selects channel 0, reflecting the single Timer W
piezo output. The regression requires that channel and replacement behavior.
Native score loading, tempo, pitch, legato and separator rules remain in `pw`.

All 176 final cases passed across the 16 HGSS scores: cold starts, warm
repeats primed by a preceding real score, 0/8/20/80/200 ms initial stalls,
and an 80 ms stall while a note was already sounding. Tone lifetimes and
separators agree with unstalled playback within the host's 200 µs comparison
tolerance. Existing 16 display/power/fault lifecycle cases also pass.

## Hardware confirmation and installation

Bench-only `G` loads the current EEPROM move score, hands its workspace to
native `BeepTick`, and inserts an 80 ms foreground stall. It does not navigate
or replace sound settings. `sound_stall_probe.py` recorded **20/20** complete
cues on one boot, including warm repeats:

- 20 tone starts and 20 measured completions; zero tone/begin/power errors.
- Shortest actual tone command lifetime: **52,667 µs**.
- One I2S startup for the batch; amplifier hold reused across cues.
- Codec/I2S ready flags returned to zero after the final hold expired.

This establishes that the tested cues survive scheduling and reach the speaker
request path for their real durations. Audible quality was not measured. The
probe stays awake on USB; it makes no sleep-current or battery-life claim.
Bench commands are excluded from production.

Before flashing, the entire storage partition was backed up and its CRC-valid
snapshots/journal independently reconstructed. After the hardware batch, all
65,536 logical EEPROM bytes matched exactly, including the paired Pokémon:

    5ccd615bca4bd063098e94bbab297c38b22475b64093869eefad74f2d421aac4

The filesystem changed only through checkpointing its one existing journal
record (generation 567→568). The production app was written only at `0x10000`;
the fresh post-test filesystem also verified against flash after that write.
Private save files and binaries remain in ignored `stick/.build` storage.

Frozen production app SHA-256:

    d9e3c352cf217a292557c099923fd2c7c3fa75c28a5db3be5059a2336d90aef3

The manifest verifies identical complete machine bodies and addresses for
`measure_ir_gate`, `aligned_next` and `run_gate_segment` against the frozen
production baseline. No new optical exchange qualification is claimed.
The final production boot/liveness check is recorded with the installation
artifacts in `stick/.build/sound-scheduling-2026-09-26/`.
After USB control-line release, CPU0 readback found descriptor `0xabcd5432`,
adapter ready=1 and RTC advancing `0x3249613e`→`0x32496140` across a 1.5-second
resume. The debugger resumed the chip on exit; no further reset followed.
