# Native execution and Stick services

The behavioral reference is the [matching Pokéwalker reconstruction](https://github.com/lumirth/pw/tree/6dc7bc09950078fa3fe0dffa4dae34e9549a99da).
Both supported H8 compiler suites must continue producing the reference image.
Stick-specific branches adapt hardware and interaction; they do not replace
native game rules, rendering, score interpretation, or motion estimation.
DS-side byte order and completion behavior were also checked against locally
supplied HGSS source. That source and its artwork are not distributed here.

## Execution ownership

`native_runtime.cpp` owns Timer B1, Timer W, RTC events, Center IRQ, interrupt
masks, and foreground wake eligibility. Timer W deadlines retain 32768 Hz
watch-clock precision. The native score interpreter runs from compare events,
not the physical speaker service or the former fixed 8 ms BeepTick scheduler.

RTC time-of-day advances independently of interrupt delivery and the native
saved-seconds counter. Pending RTC sources coalesce while masked; flag state is
separate from interrupt enable. Interrupt effects delivered during foreground
execution do not themselves request another foreground run. Timer B1 follows
native motion transitions, not an input queue's desire for faster consumption.

The application task owns native state. Interrupt service occurs at runtime and
adapter boundaries, including LCD writes; output workers do not call game
updates or mutate the motion/sound/IR workspace. This is a source-level native
platform implementation, not instruction-accurate H8 CPU emulation.

`board_runtime.cpp` composes services and chooses the next deadline. Settings
owns its overlay in `board_settings.cpp`; diagnostic commands live in
`board_diagnostics.cpp` and compile out of production. Native renderers still
advance only when the firmware consumes a refresh request.

## Inputs and interaction

Physical acquisition and Settings share debounce state. PMIC L supplies a
current level and retained press; configuration/read failures are retried.
The four layout encodings and migration rules remain unchanged.

The delivery queue timestamps transitions. Disjoint buttons may omit a redundant
neutral state, but repeated presses of one button require a delivered release.
Center IRQ becomes eligible only at the delivery front, so an older direction
cannot merge with a later confirmation. Pending transitions older than 250 ms
or a full queue cancel the gesture and require physical release. Diagnostic
counters expose both cases; native scans are not manufactured to drain a queue.

While dark, M presents native Center activity. Native hold qualification starts
interactive mode and requests the original social-offer check. Panel and audio
preparation start earlier without claiming the native score workspace. Wake,
overlay, and IR transitions consume their physical gesture through release.
Opening Settings cancels an undelivered Center latch. Closing it resumes sampling
and visibility without pretending another native Center wake occurred.

Two-key layouts retain dedicated L access to Stick Settings. Three-key layouts
add Stick Settings after Contrast in the native Settings selector. Holding a
gameplay direction no longer opens the overlay. Settings captures buttons, but
native time, motion and eligible foreground work continue; it is not a game pause.

## Output and power

The LCD bus publishes a revision for visible RAM, start-line and power changes.
The presenter consumes an immutable 96-by-64 snapshot, not `uiFrame` or live game
state. Each call transfers at most eight native rows, returns with SPI idle, and
retains no caller pointer. A newer snapshot can supersede remaining strips.
Appearance, rotation and overlay invalidation also require presentation even
without a new native LCD revision.

Codec/I2S preparation is independent of Timer W. Output remains prepared during
interactive use and is suspended for screen-off carry and IR. Readiness means
configuration succeeded, not that acoustic settling was measured. The existing
single-voice waveform, codec readback, rail-generation handling and teardown
protections remain. Digital PCM checks do not establish analog onset or power.

Sleep deadlines include native events, required input polling, Settings
inactivity, and display/audio/supply recovery. Quiet dark M-only input uses GPIO
wake; when CPU sleep is unavailable, polling still detects M. Digital GPIO
ownership is restored after every sleep attempt. Powered I2S is an actual sleep
constraint, distinct from native score ownership.

## Infrared handoff

Native framing, tokens, checksums, commands and completion actions are retained.
Optical acquisition retains its dedicated implementation and 240 MHz requirement.
Audio is quiesced and the prepared Connect image presented before acquisition.

A completed non-self burst hands execution to the protocol with the receiver
core's interrupts restored. The worker waits while decoding and bounded storage
work complete. Consumed-event tickets prevent lost or premature release. Startup
and transmission wait for actual aligned acquisition before emitting a bit;
that preparation is not tagged as self-echo. Ignored or rejected input releases
reception when no native packet remains. Stop wakes a waiting receiver and waits
for completion and suspension before deleting its task.

Native-requested bank changes can be presented in bounded strips in the
receive-to-reply window. Acquisition coordination changed and must be physically
requalified: simulator success is not proof of unchanged optical timing.

## Persistence

The native 64 KiB EEPROM address space, mixed-endian records, mirrors and staging
regions remain intact. The status path explicitly writes big-endian Watts, as
required by the DS reader.

The v2 backend uses CRC-protected checkpoint and journal headers with generation
and sequence numbers. A published checkpoint covers earlier records even when
a reset precedes journal deletion. Reads observe accepted writes immediately;
mirrored logical writes remain grouped. Writes check write, sync and close
results and independently read back records.

Legacy snapshots and journals migrate using their existing recovery
interpretation, retaining their original files. Legacy records cannot prove
whether a journal belongs before or after a checkpoint; migration cannot
reconstruct already-lost history. New-format corruption fails closed rather
than silently falling back to older legacy progress.

IR no longer defers its entire session in RAM. It prepares journal capacity
before acquisition, appends bounded durable records during the reply window,
and permits checkpoint compaction after the session. Actual persistence failures
suppress further acknowledgments and pending activation; completed durable
writes are not rolled back. Timing overruns are recorded separately from storage
failures. LittleFS and flash latency still need measurement against peer deadlines.

The DS's Back and present-collection paths do not wait for a final deletion
acknowledgment. No new handshake or claim of two-device atomicity is introduced.
Native staging and completion order retain their existing recovery limits.

## Qualification

Run the [production firmware workflows](../simulation/README.md), native-source
formatting/static analysis, and exact H8 matching under both supported compilers.
Public EEPROM fixtures exercise normal startup and persisted flash, not game
memory hooks. Physical controls, acoustics, walking comparisons, current draw,
flash interruption, and real peer/Game Card exchanges need hardware qualification
on the exact candidate.
