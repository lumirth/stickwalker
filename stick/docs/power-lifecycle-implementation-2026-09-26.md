# Peripheral lifecycle and sleep implementation — 2026-09-26

Original code-only milestone: implemented, built and host-verified candidate.
Subsequent hardware work is recorded in
[power hardware validation](power-hardware-validation-2026-09-26.md), including
failed trials, fixes and final installation. No supply-current measurement has
been made. The user confirmed the design before these firmware edits.

## Implemented changes

- Shared LCD/codec/microphone supply ownership prevents either display or sound
  from switching off the other. Remove the supply after both release it; retry
  transient PMIC failures. Disconnect output pads at supply-off.
- Native display-off now sends ST7789 Sleep-in, releases SPI and disables the
  backlight timer. Supply removal loses physical controller RAM, while the
  native virtual image, contrast and orientation remain retained.
- Display reset and Sleep-out delays are asynchronous deadlines. Reuse the
  installed ST7789 driver's analog/gamma command prefix; retain its 8 ms reset,
  64 ms reset-recovery and 130 ms Sleep-out timing. Prepare during the existing
  wake gesture, with no new application state transition or lit-screen time.
- Keep the verified brightness curve and four shades. Configure backlight PWM
  with RC_FAST and KEEP_ALIVE so CPU Light-sleep can preserve a visible image.
  Native render calls and game cadence remain intact; transfer only the dirty
  bounding rectangle and skip identical images. Invalidate after overlay use,
  rotation or controller power loss. Track PWM allocation separately from its
  last successful update so an update failure cannot skip idle timer teardown.
- Suspend the ES8311 even before its first score; snapshot the registers changed
  by suspend for exact warm configuration restore. End I2S and remove the audio
  ownership claim after the existing 500 ms amplifier hold. Preserve the 65 ms
  startup allowance, pitch, waveform, note timing and volume mapping. A failed
  test-tone start also tears down output instead of retaining I2S indefinitely.
- Production discards bounded unread serial input instead of using it as a
  permanent sleep veto. Bench command behavior remains behind its existing flag.
- Retry failed sleep initialization, timer setup and entry at a bounded rate.
  Log initial setup failure instead of silently accepting it forever.
- Permit idle processor sleep during game presentation and the board overlay,
  with active gestures, open audio streams and IR keeping their timing.
- Close abandoned Stick Settings after 90 seconds without a new menu input.
  Automatic close does not wake an expired native display; normal manual close
  retains its existing wake behavior.
- Use 80 MHz outside IR, retaining the normal peripheral bus clock. Restore
  240 MHz in IR configure before sampler startup; reduce frequency only after
  the worker is known to have stopped.
- Disable the TX RMT channel between sessions, re-enable its retained allocation
  on the next session, and clean up partial channel/encoder creation failures.

The native source under src/ and include/ was not edited in this milestone.
Native menu/game sampling continues, while sound/IR ownership gaps remain.

The PWM configuration follows
[Espressif's Light-sleep support](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/peripherals/ledc.html);
codec suspend follows the register sequence in
[Espressif's ES8311 driver](https://github.com/espressif/esp-adf/blob/release/v2.x/components/esp_codec_dev/device/es8311/es8311.c).

## Verification

Run:

    python3 stick/audits/idle_power_trace.py
    python3 stick/tests/ir_tx_power_test.py
    python3 stick/build_port.py
    python3 stick/build_port.py --bench-control

The lifecycle probe compiles unchanged production adapter code against electrical
boundary stubs. Eleven cases pass: quiet, unread serial, transient sleep setup
failure, persistent setup failure, failed sleep entry, visible image, moving
cadence, rail-off failure, codec suspend failure, rail-on failure and speaker
startup failure. Each case additionally exercises amplifier startup/hold,
cold/warm codec resume, display/sound shared ownership, asynchronous LCD
restoration, native pixel retention, identical/partial/full transfers, a complete
overlay cursor cycle, manual close, abandoned-overlay shutdown, failed PWM
update teardown and failed test-tone teardown.

In the quiet 1.2-second modeled interval, the LCD receives Sleep-in, the shared
supply is off, unread serial is drained, and successful sleeps occur. After the
audio hold, I2S is ended and the codec receives the 15 suspend writes. A lit
image permits CPU sleep. The moving case runs 19 main ticks in the same modeled
interval; sensor configuration and the original motion schedule are unchanged.

The separate TX test compiles actual ir_tx.cpp. Normal operation, encoder
creation failure and enable failure pass. It verifies reusable allocation,
disable/re-enable across sessions, rejection while disabled, all 136-byte
transmit symbols, transport XOR, 1.625 us pulses and rational UART cell timing.

These checks establish lifecycle and scheduling contracts, not battery current,
audible sound, physical PWM stability, walking accuracy or live optical reception.

## Frozen artifacts

The production candidate app SHA-256 is:

    eee3a52ed7f9f87840068523c53f69f0b8dcee677d973376860586acf015a423

The diagnostic candidate app SHA-256 is:

    49481eb71eda176445d36cbc6d8d9d264d6a22e51c80046884d1a1dd705c834b

Local artifacts under stick/.build/:

- power-lifecycle-production-app.bin / .elf
- power-lifecycle-bench-app.bin / .elf
- power-lifecycle-production-manifest.json (145 source/header hashes)
- power-lifecycle-production-build.log / power-lifecycle-bench-build.log
- power-lifecycle-host-trace.json
- power-lifecycle-hotloop.dis / power-lifecycle-hotloop-comparison.json
- power-baseline-full-{measure_ir_gate,aligned_next,run_gate_segment}.bin / .dis

The previous production sound-amp-hold image remains unchanged:

    20afaff5c8d0740eacffdd2b896536324fd4aa204fca83655e495374dc041f5b

All three complete function bodies in the production candidate are exactly
byte-identical to that frozen app, including their addresses:

| Function | Address | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| measure_ir_gate | 40375534 | 247 | 27287860503e81b64e854db6b8df96c791ef399b2bb49eece6b673ee70c8c21d |
| aligned_next | 4037562c | 95 | b15e5c4bdc7d67cc228e7812ece6d65995b7d8cf8dabe4dee8de44dc6e711744 |
| run_gate_segment | 4037568c | 499 | b5d17417963997781a73b50e74229c584083193cd8a456650a8e489c0b2f7bbd |

The older sound-power-baseline-hotloop.dis was from a different bench ELF and
truncated the producer. A preliminary comparison against that file was
inconclusive. The final comparison above uses complete bytes located in the
actual frozen production app. Unchanged instructions do not establish unchanged
optical behavior when peripheral supplies and clocks change.

No device access, flash, reset, or paired Pokémon storage access occurred in
that original code-only milestone. Subsequent hardware access is recorded in
the linked hardware validation ledger.

## Remaining work and hardware boundary

The 100 ms PMIC polling cap, RTC peripheral retention, current 100 Hz BMI270
normal mode, and 16 Hz moving wake cadence remain. Bounded FIFO batching,
lower-power sensor modes and any justified checkpoint-based deeper sleep remain
pending; no month, two-week or one-week runtime is claimed.

The PMIC datasheet identifies a plausible cause of the prior stuck IRQ: its
WAKE_SRC makes IRQ Status 3 bit 1 persist until cleared. GPIO/system events
also need selective masking and clearing. This supports a focused next IRQ
investigation; it does not prove the cause of the saved hardware rejection or
establish press-onset/hold timing. The successful polling fallback remains
enabled until that behavior can be validated.
[M5PM1 datasheet, pp. 22–24](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_CN.pdf).

At the original code-only milestone, the device was unavailable. Remaining
qualification needs
battery-powered peripheral/wake/sound checks, a complete faithful IR exchange,
and whole-device supply-current/energy measurements. Sensor rate/filter choices
also require acceleration evidence to validate walking accuracy and onset.
Those measurements are needed to select the remaining optimizations and
qualify lifetime under the agreed heavier-use planning scenarios.
