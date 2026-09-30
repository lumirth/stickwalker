# Battery and native behavior: design discussion

Status: the user confirmed shared understanding and implementation is underway.
Faithful screen availability, experience fidelity, lifetime priorities and
native sampling-gap policy are settled. The first lifecycle/sleep candidate
has passed host regression checks and both firmware builds; hardware validation
and achieved lifetime remain open. See [the implementation record](power-lifecycle-implementation-2026-09-26.md).

## Existing constraints

- The matching `pw` source remains authoritative for game and protocol behavior.
- Preserve game data, walking progression, gameplay timing, and retail IR compatibility.
- Prefer the native motion estimator while adapting sensor inputs and timing to
  the BMI270. Matching chip rates is not a product requirement.
- Preserve the original screen-availability experience. The native 60/90-second
  policy is the reference, rather than a demand to reproduce every numeric
  hardware setting. Aggressively shortened screen availability is not a way to
  obtain a favorable battery claim.
- The existing device and placement are the working hardware.
- This discussion follows the [battery audit](battery-life-audit-2026-09-26.md).

## Refresh: three different operations

1. **Native UI cadence:** original and port normally render at 4 Hz through
   the quarter-second pending event. Rendering can advance battle and Dowsing
   state, so reducing native render calls can slow gameplay.
2. **TFT transfer:** at the audited baseline, each presented frame expands the 96×64 native
   image to 192×128 RGB565 and sends all 49,152 pixel bytes. At four frames/s,
   that is 196,608 bytes/s. A new UI frame number can trigger a transfer even
   when the resulting pixels are identical. Dirty image comparison and partial
   transfers preserve native rendering while reducing port work in the new candidate.
3. **Panel scan:** the ST7789P3 default implies approximately 60 Hz internal
   scanning. The original fallback NT7508 initialization implies approximately
   70.6 Hz; EEPROM can override that initialization. These are source/datasheet
   inferences, not measured scan rates. There is no evidence of a faster port
   scan rate. The ST7789P3 normal-mode table offers rates down to 39 Hz, with
   visual quality and actual savings still to establish.

Sources: native RTC refresh (local evidence: `/Users/lu/Desktop/pw-release/pw/src/application/pw_rtc.c`),
native render dispatch (local evidence: `/Users/lu/Desktop/pw-release/pw/src/application/pw_main.c`),
[port presentation gate](../board_runtime.cpp),
[port frame expansion](../display_panel.cpp),
original fallback initialization (local evidence: `/Users/lu/Desktop/pw-release/pw/src/application/pw_nt7508.c`),
[local TFT initialization](https://github.com/m5stack/M5GFX/blob/0.2.29/src/lgfx/v1/panel/Panel_ST7789.hpp#L66),
[NT7508 datasheet, pp. 39, 46, 48–49](https://www.orientdisplay.com/wp-content/uploads/2022/08/NT7508_V1.0.pdf),
[ST7789P3 datasheet, pp. 244–245](https://files.waveshare.com/wiki/ESP32-S3-GEEK/ST7789P3.pdf).

The original screen timeout is 60 seconds on entering interactive mode and
90 seconds after later button edges. The port keeps that policy. At the audited baseline, the Stick
Settings overlay had no inactivity timeout. The new candidate closes that
additional overlay after 90 seconds without an input, returning to the native
screen state without waking an expired display. Ten brief checks can therefore
cost many minutes of lit-screen operation, beyond the time spent pressing buttons.
Native timeout constants (local evidence: `/Users/lu/Desktop/pw-release/pw/include/application/pw_power.h`),
button timeout reset (local evidence: `/Users/lu/Desktop/pw-release/pw/src/application/pw_player_input.c`).

These values are native countdown policy rather than unconditional wall-clock
cutoffs. A held level does not refresh the countdown repeatedly; a new edge
does. Native display shutdown is checked in the main task when higher-priority
work does not take precedence. Score playback defers that check. Ordinary
games use the common policy; some factory diagnostic views continually reload
the display countdown. The original has no progressive dimming stage.
Wake recognition is also native behavior: the center hold takes eight native
input scans before entering interactive mode. Power saving should not add a
second deliberate hold or change that gesture's meaning.
Native display shutdown (local evidence: `/Users/lu/Desktop/pw-release/pw/src/application/pw_main.c`),
native sound foreground (local evidence: `/Users/lu/Desktop/pw-release/pw/src/application/pw_main.c`),
native center hold (local evidence: `/Users/lu/Desktop/pw-release/pw/src/application/pw_player_input.c`).

The user clarified that fidelity means faithfully recreating the experience,
not adherence to exact numbers on different chips. Source timings provide a
behavioral reference. Driver schedules, sample rates, buffering and sensor
power modes may differ where they preserve responsiveness and good walking
accuracy. Source quirks such as coupling a button edge to the motion ring index
are not automatically features the port must intentionally reproduce.

An existing visual deviation also needs to remain explicit: virtual connection
bank changes are retained, but the port does not upload them during IR reception.
The physical connection animation therefore freezes. Restoring it requires
safe presentation scheduling around the receiver and reply deadlines.
[IR foreground branch](../board_runtime.cpp).

## Candidate improvements

| Area | Candidate | Native-behavior considerations |
| --- | --- | --- |
| Inactive peripherals | LCD Sleep-in, codec suspend, I2S end, shared supply gating | Complete the missing hardware lifecycle; retain virtual image and scores. |
| Sleep reliability | Remove unread-production-serial veto; recover sleep errors | Correctness fixes without intended game changes. |
| Background wakes | Proper power-button interrupt instead of 100 ms polling | Retain button gestures; handle interrupt masking and release correctly. |
| Interactive CPU | Sleep between work while the image remains visible; lower awake frequency | Native cadence stays intact; backlight PWM must remain stable during CPU sleep. |
| Display transfer | Skip identical images; upload changed regions; convert in smaller buffers | Continue native render calls even when no physical transfer is needed. |
| Panel scan | Lower TFT scan rate independently of game updates | Needs grayscale/flicker/tearing validation; savings are not assumed linear. |
| Display visibility | Efficient backlight mapping at the user's original contrast setting | Preserve four shades and the settled native availability policy; earlier automatic dim/off is excluded. |
| Motion sensor | Lower-power BMI270 configuration and appropriate filtering | Preserve good walking detection and estimator units/bandwidth; numeric hardware rate may differ. |
| Moving carry | FIFO batches of actual acceleration with correct time mapping | Prefer the native estimator; preserve its temporal meaning or explicitly validate adaptations. |
| Stationary carry | Longer sleep with suitable native/sensor activity detection | Motion interrupts can assist; onset latency and missed walking must be evaluated. |
| Retained resources | RTC-domain reduction, idle RMT cleanup, PMIC idle sleep, memory/build audit | Resource changes must preserve wake and receiver behavior. |
| Audio | Full idle teardown; tune volume/brief hold without losing short cues | Preserve pitch/score timing and the verified amplifier startup behavior. |
| IR | Restore 240 MHz before sampler startup; keep full-speed session path | Existing optical waveform and retail timing remain protected. |
| Deep sleep | Dedicated retained checkpoint and elapsed-time resume | Current EEPROM is not a complete runtime snapshot; normal boot is not resume. |
| Supply-off carry | IMU/PMIC retained with ESP off | Current board supports motion repower, but faithful FIFO/time/state continuation is unsolved. |
| Low-power coprocessor | Clock/event bookkeeping while the main CPU sleeps | Direct raw IMU reads are not available on the stock sensor wiring through its RTC I2C interface. |

The audited CPU sleep policy excluded a lit display. The new candidate
redesigns that condition: displaying a stored image does not require the processor to
remain continuously awake. ESP-IDF supports continuing backlight PWM during
Light-sleep using a compatible clock and keep-alive configuration, at some
extra sleep current. It is not safe simply to remove the display predicate:
the existing PWM configuration stops output during sleep.
[Current sleep policy](../board_runtime.cpp),
[LEDC clock and sleep behavior](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/peripherals/ledc.html).

The ULP coprocessor is not an automatic replacement for the main sensor reader.
Its RTC I2C interface supports SDA on GPIO1/3 and SCL on GPIO0/2, whereas the
Stick's BMI270 bus is wired to GPIO47/48. RTC GPIO access likewise does not cover
those two pins. Low-power event/clock work remains possible, but an unmodified
board still needs main-processor participation to retrieve raw IMU samples.
[Espressif ULP access and RTC I2C pins](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/ulp-risc-v.html),
[Stick S3 sensor pin map](https://docs.m5stack.com/en/core/StickS3).

FIFO batching is a substantial design change rather than a sensor toggle.
Native sample ticks also handle step pacing, inputs, view/PRNG state, and
ownership transitions. RTC minute/hour flags coalesce. Batches must preserve
chronological sample/RTC processing and flush before input or an IR transition.
Audio and IR own shared workspace and suspend native sampling. The user chose
to preserve the original gaps as faithful behavior. FIFO batching must not
replay or credit samples from those excluded intervals. Ordinary menus and
games still sample through MainTick; this choice does not introduce a blanket
interaction gap. See [the motion ownership decision](../../docs/adr/0001-preserve-native-motion-ownership.md).
BMI270 hardware rates do not include exactly 16 Hz, so blindly feeding all
FIFO records changes the estimator's temporal scale. Low-power filtering
also needs validation against the required signal bandwidth.
Native MainTick (local evidence: `/Users/lu/Desktop/pw-release/pw/src/application/pw_main.c`),
native motion processing (local evidence: `/Users/lu/Desktop/pw-release/pw/src/application/pw_pedometer.c`),
native RTC dispatch (local evidence: `/Users/lu/Desktop/pw-release/pw/src/application/pw_rtc.c`),
[BMI270 FIFO and sensor-time documentation](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf).

## Battery budgets

The user settled the lifetime priorities:

- Aim for one month between charges.
- Two weeks is the minimum useful result.
- One week is the absolute floor.

These are whole-device targets for representative everyday use, including
movement, brief checks, games, idle-on screen tails, sound and occasional IR.
They are design requirements, not achieved lifetime estimates. Qualification
must state its use profile and usable battery capacity; the targets do not imply
one week under unlimited continuous screen-on play. The heavier planning cases
below remain provisional engineering brackets.

For the nominal 250 mAh battery, before capacity loss or reserve:

| Whole-device lifetime | Maximum average battery current |
| --- | ---: |
| 2 days | 5.208 mA |
| 7 days | 1.488 mA |
| 14 days | 0.744 mA |
| 30 days | 0.347 mA |
| 60 days | 0.174 mA |
| 90 days | 0.116 mA |

M5Stack publishes reference states of 102.4 µA for L2 and 52.47 µA for L1.
These establish a credible low-power hardware scale, not achieved Stickwalker
currents. L1 repowers/reboots the ESP on wake; it does not preserve ordinary RAM.
[M5Stack capacity and reference currents](https://docs.m5stack.com/en/core/StickS3),
[M5Stack sleep/repower examples](https://docs.m5stack.com/en/arduino/m5sticks3/m5pm1).

The following optimistic illustration assumes 0.10 mA for all carry time and
50 mA during interactive use. Those are chosen model inputs, not measurements.
Additional motion-processing, sound and IR energy are excluded:

| Lit interactive time/day | Ideal lifetime |
| --- | ---: |
| 1 minute | 77.4 days |
| 5 minutes | 38.1 days |
| 10 minutes | 23.3 days |
| 30 minutes | 9.1 days |

Equation: daily mAh = carry_mA × (24 − interactive_hours) +
interactive_mA × interactive_hours. Lifetime_days = 250 / daily_mAh.

This does not establish a 0.10 mA moving-carry implementation. It shows why
months require both low background current and low interactive energy. CPU
sleep during visible UI can reduce the interactive cost as well as stationary
carry optimizations reducing the background cost. Battery aging, capacity,
voltage conversion, and self-discharge reduce the ideal results.

Engineering assessment: days are a strong first target after completing the
existing shutdown design; weeks are a credible goal with wake and motion
processing improvements; a month is plausible under an efficient, brief-use
profile. Multiple months of faithful everyday use remain an ambitious target
requiring a demonstrated whole-device microamp budget. A reference sleep
current alone cannot support that claim.

## Second-round use profile and motion concerns

The user expects more movement and more checks/gameplay than the first suggested
profile, with connections less frequent than daily. Use heavier planning cases
rather than minimizing screen use to obtain a favorable battery estimate.
Provisional brackets are six to eight hours of motion-mode operation and
30–60 minutes of total lit interaction per day, with several IR sessions per
week. These numbers bracket the user's direction; they are not yet agreed
requirements or measured usage. Total lit time includes the native idle-on tails.

Interactive efficiency becomes especially important. For illustration, using
six hours of moving carry at 0.30 mA, 17.5 hours of stationary carry at 0.10 mA,
and half an hour of interactive use gives:

| Chosen interactive current | Ideal lifetime on 250 mAh |
| --- | ---: |
| 50 mA | 8.8 days |
| 20 mA | 18.5 days |
| 15 mA | 22.6 days |

All currents here are model inputs, not measured or proven achievable values;
extra sound/IR energy, reserve and capacity losses are omitted. A faithful
screen-availability policy is compatible with lower interactive energy through
CPU sleep, transfer reduction and appropriate hardware drivers. It does not
remove the TFT/backlight's energy cost.

### Can opening the screen lose steps with batching?

Separate raw samples, an accepted/paced step budget, and already credited totals.
The native estimator analyzes 64-sample windows at a 16 Hz timeline. A new
button edge resets `sampleIndex` before the FFT eligibility test; the source
does not erase the ring array or clear the accepted `stepPacing` budget there.
Already credited steps survive. An incomplete analysis window does not carry a
guarantee that every recent physical footfall will eventually be credited,
including in the original firmware.

A deferred implementation can create **additional** losses if it clears or
reinitializes the FIFO/ring at wake, overflows storage, timestamps incorrectly,
or feeds a newly pressed button into old virtual ticks. It need not create
those losses. The proposed safe ordering is:

1. Record the wake/input event and its position in the sample timeline.
2. Drain retained raw samples into separate bounded storage without resetting
   native motion state or the sensor first.
3. Replay earlier sample and RTC events with their historical input state.
4. Insert the real input at its native scan boundary, preserving capture-before-
   input ordering, ring-index reset, FFT eligibility and existing step pacing.
5. Continue live interactive processing. Respect audio/IR ownership transitions
   and never process future samples ahead of the button event.

"Catch up before input" means process the history chronologically up to that
input. It does not mean force an FFT on a partial window or credit steps earlier
than the native pacing rules allow.

Scheduling validation for batching should compare immediate and deferred
execution of the chosen motion design on the same timestamped samples/events,
including button presses at
every ring position, ongoing pacing, inactive-to-moving transitions, hour/day
boundaries, and audio/IR handoffs. Expected result: exact agreement with that
immediate-processing reference and no additional dropped samples. Product
validation additionally needs walking accuracy, wake responsiveness, and game
compatibility. Retaining the estimator alone does not prove identical physical-
device step accuracy, and literal source quirks are not the product goal.

The BMI270's 2,048-byte FIFO holds at most about 292 seven-byte accel frames,
before metadata and safety margin: about 2.9 seconds at the current 100 Hz or
11.7 seconds at a proposed 25 Hz. Lowering ODR and introducing batching together
would confound sensor-input changes with scheduling changes. A conservative
candidate would first keep the existing sensor mode and use short batches,
then investigate lower-power sensing independently.
Native capture/input/FFT order (local evidence: `/Users/lu/Desktop/pw-release/pw/src/application/pw_main.c`),
native input ring reset (local evidence: `/Users/lu/Desktop/pw-release/pw/src/application/pw_player_input.c`),
native step pacing (local evidence: `/Users/lu/Desktop/pw-release/pw/src/support/lib_common.c`),
[Bosch FIFO and ODR documentation](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf).

## Later decision: immediate motion and M-only dark wake

The user subsequently declined deferred FIFO motion batching unless the
original firmware used it. Keep immediate 1 Hz inactive/activity checks and
16 Hz moving/interactive sampling, including the original audio/IR gaps. The
batching discussion above records an earlier candidate, not an implementation
mandate. Physical M alone begins the existing 500 ms screen-off wake; screen-off
L/R do not poll, wake the CPU, prewarm the panel, or replay inputs after wake.
Visible controls retain L responsiveness. See
[the wake decision](../../docs/adr/0002-m-only-dark-wake.md).

## Proposed implementation sequence

1. Complete idle LCD/audio/I2S/supply shutdown and fix the serial veto and
   unrecoverable sleep-setup failure. Preserve reliable short sound cues and saves.
2. Replace unnecessary periodic wakes with appropriate events; permit processor
   sleep between visible UI work with stable backlight PWM and immediate controls.
   Keep native rendering/game updates, skip identical physical transfers, and
   validate lower active clock settings outside the protected IR path.
3. Preserve immediate native motion processing and its ownership gaps. Evaluate
   lower-power sensor configurations separately against walking accuracy; do not
   implement deferred FIFO batching.
4. Evaluate whole-device energy for representative heavier use, including idle-on
   screen tails. Compare against the month/two-week/one-week priorities with
   capacity headroom. The provisional six-to-eight-hour movement and 30–60-minute
   lit-use brackets are planning scenarios rather than a claimed use guarantee.
5. Introduce checkpoint-based deeper sleep only if the retained-state design
   cannot meet the budget and a correct resume path is justified. Normal boot is
   not a substitute for faithful continuation.

Validation separates chronological motion-processing correctness, physical
walking accuracy, control/display/sound experience, retail IR compatibility,
and whole-device energy. Code inspection resolves lifecycle and scheduling
issues; achieved lifetime requires subsequent hardware current/energy evidence.

## Design tree: settled product choices

1. **Use profile:** heavier movement and interactive use, fewer connections
   than daily. Direction settled; quantitative planning brackets remain provisional.
   - Lifetime priorities settled: one month aim, two weeks minimum useful,
     one week absolute floor. Use conservative energy accounting across all modes.
2. **Presentation policy:** preserve native screen availability experience. Settled.
   - Engineering constraint: hardware restoration should fit the normal wake
     interaction without adding a noticeable second wait. No chip-level timing
     decision is being delegated to the user.
3. **Background motion:** experience fidelity permits adapting the different
   sensor. The user asks for comparable accuracy and no added wake-related step
   loss; no unconditional acceptance of a particular batching design is inferred.
   - Native sampling gaps settled: preserve gaps where the original leaves
     them, particularly sound/IR ownership. Ordinary menu/game sampling continues.
     Deferred FIFO batching was subsequently declined; samples remain immediately
     processed on the existing schedules.
   - Engineering work: sensor configuration, retained Light-sleep
     and any necessary checkpoint-based deep sleep, assessed against the settled
     experience and energy constraints.

Existing game/protocol authority is settled. The native estimator is the
preferred starting point; hardware rates and buffering are engineering choices
to validate against the experience goal. The user confirmed this shared plan;
implementation and validation now follow the sequence above.
