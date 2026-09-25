# StickS3 power and background motion: source notes

These are manufacturer specifications and design options, **not measurements of the Pokéwalker port**. The port's runtime, charge use, step fidelity, and wake behavior need tests on the actual firmware and board.

## Comparison point

- Nintendo specifies one CR2032, about **0.2 mW while walking** and **3 mW with its LCD lit**. Its approximately **four-month** battery claim assumes about 30 minutes of game use and 10,000 steps each day at 25 °C; Nintendo says temperature and use alter that duration. At a nominal 3 V, 0.2 mW corresponds to about 67 µA, but that conversion is only a comparison of power and voltage, not a measured average current. [Nintendo Pokéwalker specifications](https://www.nintendo.co.jp/ds/ipkj/qa/catC.html); [Nintendo operations manual](https://csassets.nintendo.com/noaext/image/private/t_KA_PDF/Pokewalker_Tri?_a=BATCtdAA0).
- The StickS3 has a **250 mAh rechargeable battery**. M5Stack publishes these whole-device points at a 4.2 V supply: power off **14.02 µA**, L1 **52.47 µA**, L2 **102.40 µA**, L3A **36.69 mA**, and full load **519.02 mA**. The published table does not specify the display brightness, sensor configuration, radio state, or application workload for each point. The numbers must therefore not be presented as current drawn by this port. [M5Stack StickS3 specifications](https://docs.m5stack.com/en/core/StickS3).

## What the board can switch off

M5Stack documents its M5PM1 power levels as independently supplied from L0. L0 keeps the power manager alive; L1 supplies the BMI270; L2 retains power for the ESP32-S3 while it sleeps; L3A is active ESP32-S3 operation. L3B supplies other peripherals, including LCD backlight, microphone, and speaker. The infrared and expansion 5 V supply is switched separately with `EXT_5V_EN`. Thus turning off only the LCD backlight does not imply that the processor, audio rail, or IR rail stopped drawing power. [M5Stack low-power configuration](https://docs.m5stack.com/en/arduino/m5sticks3/m5pm1).

The BMI270 `INT1` is wired to M5PM1 `PYG4`; the PMIC can route an interrupt to ESP32-S3 **GPIO13** or wake and repower the ESP32-S3 after an L1 shutdown. M5Stack provides examples of both the L1 sensor wake and ESP32 sleep wake paths. In the L1 shutdown path the ESP32 boots again; a port using it must preserve game state and recover motion history across that reset. [M5Stack low-power configuration, IMU examples](https://docs.m5stack.com/en/arduino/m5sticks3/m5pm1); [M5Stack StickS3 pin map](https://docs.m5stack.com/en/core/StickS3).

The ESP32-S3 can enter Light-sleep with RAM and CPU state retained, or Deep-sleep with most RAM and digital peripherals off. Its external interrupt wake source can use GPIO13 because ESP32-S3 RTC GPIOs include 0–21. ESP-IDF's automatic Light-sleep requires power management and tickless idle to be enabled, and frequency/power management locks held by drivers can prevent entry. This is a possible implementation route, not evidence that the current Arduino build enters either sleep state. [Espressif sleep modes](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32s3/api-reference/system/sleep_modes.html); [Espressif power management](https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/system/power_management.html).

## What the motion chip can do while the processor rests

The BMI270 supports accelerometer-only low-power sensing, motion interrupts, a step counter and detector, and a **2,048-byte FIFO** with full/watermark interrupts. Bosch gives **10 µA typical** for accelerometer-only low-power sensing at 25 Hz and **210 µA typical** for accelerometer-only normal mode; the power-mode table gives down to **4 µA** at other low-power configurations. These are **chip currents at Bosch's test conditions**, not the StickS3's battery current. [Bosch BMI270 datasheet, pp. 11 and 26–27](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf).

The FIFO can retain accelerometer samples while the ESP32 sleeps and signal a watermark over the available interrupt route. That may allow the original Pokéwalker step algorithm to consume a batch of real samples after wake instead of running a processor poll for each sample. The FIFO's limited size, selected output rate, sample timestamps, overflow policy, and the original algorithm's timing assumptions must all be checked. Bosch's hardware step counter is another low-power option, but substituting it for the original decomp's algorithm would change step-count semantics and needs direct comparative validation. [Bosch BMI270 datasheet, FIFO and interrupt sections](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf); [M5Stack interrupt wiring](https://docs.m5stack.com/en/arduino/m5sticks3/m5pm1).

## Illustrative battery arithmetic

An ideal 250 mAh battery spread across 120 days permits only **86.8 µA average** (`250 / (120 × 24)`). That is a target budget, not a runtime prediction. At the published M5Stack state currents, uninterrupted L1 would calculate to **198 days**, uninterrupted L2 to **102 days**, and uninterrupted L3A to **6.8 hours**. Actual usable capacity and board current vary, so these are deliberately idealized bounds.

If a day contained 30 minutes at the published **36.69 mA L3A** point and 23.5 hours at **102.40 µA L2**, the arithmetic yields about **20.8 mAh/day** and **12 days** from 250 mAh. The active period alone costs 18.3 mAh/day. L3A does not establish the current of the actual screen-on port, which may also power L3B; this calculation shows why the four-month original usage claim cannot simply be transferred to this board. The source for all state currents and capacity is [M5Stack's product specification](https://docs.m5stack.com/en/core/StickS3).

## Measurements needed for a usable port

1. Measure **battery-terminal current** with USB disconnected for active UI (several brightness levels), dark-screen background walking, stationary background, IR idle/listen, and IR transfer. Record current distribution and time in each state; PMIC battery-voltage/percentage reads alone do not provide a discharge-current measurement. [M5Stack's battery API example exposes charge status, level, and voltage](https://docs.m5stack.com/en/arduino/m5sticks3/battery).
2. Confirm which rail each port path leaves enabled, then verify actual Light-sleep or PMIC L1 transitions and wake latency. Measure step count through stationary-to-walking transitions, sustained walks, and wake/sleep boundaries against the current port and, where possible, original Pokéwalker behavior. [M5Stack low-power configuration](https://docs.m5stack.com/en/arduino/m5sticks3/m5pm1); [Bosch BMI270 datasheet](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf).
3. Apply measured state currents and a stated daily-use profile to a capacity budget, then validate with a USB-disconnected discharge run. Avoid treating a voltage-derived battery percentage or the manufacturer's state table as a measured lifetime of the integrated port. [M5Stack battery API](https://docs.m5stack.com/en/arduino/m5sticks3/battery); [Nintendo's explicit use profile](https://www.nintendo.co.jp/ds/ipkj/qa/catC.html).

## Port audit, 2026-09-24

The prior port's `StickPortSetup()` set the ESP32-S3 to 240 MHz. Its idle loop
called `delay(1)` and never requested ESP32 Light-sleep or a PMIC low-power
state. The source Pokéwalker stops the regular 16 Hz sample timer when its
90-second motion timeout expires, then samples on one-second RTC wakes;
the port had continued to call `MainTick()` at 16 Hz in that inactive mode.
The panel adapter correctly turned off the LCD backlight on source display
power-save, but board startup kept external 5 V enabled and M5Unified's
BMI270 initialization left the unused gyro and temperature sensors on.

The first power patch restores one sample per second in the source inactive
mode, omits quarter-second UI refresh there, disables gyro and temperature
while keeping the existing accelerometer configuration, and gates the external
5 V rail around IR sessions. It also reads battery voltage from the PMIC
object that this port actually initializes: the prior `M5.Power` adapter had
not been initialized because this port deliberately does not call `M5.begin()`.
Device regression is recorded below. Current measurement remains a separate
gate before claiming a battery-life improvement.

The bench-controlled candidate `port-power-004-app.bin` has SHA-256
`a233dc6ab318bb723b30b3f8caa37d636424c2662dab98f962a89e1f5fa92549`.
The corresponding production build completed with bench commands excluded;
its app image has SHA-256
`daad5d1aceba790bb8ed5369a0de8019a75a869709ba937f0adac93ea1a55958`.
The production image was built, not flashed. The actual Stick continues to
run the bench-controlled candidate so further source-faithful IR trials remain
possible.

On the candidate, the PMIC reported battery voltage around 4.17–4.20 V
with USB/VBUS present, and `ext_5v=0` outside IR sessions. A bench-forced
inactive state counted six source `MainTick` calls during 5.2 seconds, with
`sample_period_us=1000000` and the external rail off. The bench Center hold
returned the source to interactive mode within 1.4 seconds; a separate long
M hold also woke the display. These observations prove scheduler and wake
behavior on USB power; they do not measure battery current.

The first power candidate completed 20/20 same-boot whole `back`/`put`
operations, then independent EEPROM readback matched all 10,430 course bytes
and both status checksums. After the wake-cadence and last-good-sample
corrections, the final candidate completed a fresh `back` and `put`, followed
by 10/10 same-boot whole operations. Independent EEPROM readback again
matched the course and both status checksums. Evidence is retained under
`stick/.build/trials/power-001-repeat/` and
`stick/.build/trials/power-004-repeat/`.

That first patch reduced needless sensor and rail activity but did **not**
make the ESP32 sleep. At that milestone, the firmware still had a foreground
loop and 5 ms button polling even with the screen off. The following section
records the subsequent Light-sleep implementation; battery current remains
to be measured.

## Retained-state Light-sleep implementation

The next port milestone adds an explicit ESP32-S3 Light-sleep call in the
screen-off `MainTick` path. The game and 64-sample motion ring remain in RAM.
The wake timer is bounded by the next original 16 Hz motion sample or
one-second inactive sample, the next RTC second, and a 100 ms maximum for the
PM1 side key. M and R also wake through RTC-capable GPIO11/12. A press cannot
be delayed by the sample cadence; after wake, the runtime runs its input poll
before sleeping again. The PM1 side key's existing latched event is read on
that timer. Sound, IR, menus, active gestures, and lit display stay awake.

The first attempt routed the PM1 button IRQ to GPIO13. On this board it
repeatedly asserted just as Light-sleep began, causing `ESP_ERR_SLEEP_REJECT`
(259) and zero completed sleeps. The port therefore leaves PM1 routing as it
was and uses the bounded timer for L; it keeps direct GPIO wake for M and R.
Automatic Light-sleep is disabled while VBUS is present because USB
Serial/JTAG disconnects during it. The bench-only `Y` command permits a
five-second USB-powered sleep trial, after which normal USB operation resumes.

The flashed bench image is `stick/.build/port-sleep-001-bench-app.bin` (SHA-256
`5d59cd016fd5d34287909f96d0651b3ec482dc8d5c99f011675fad589055cf60`).
The production image was also built as `port-sleep-001-production-app.bin`
(SHA-256 `cba9d5fbef9c3f37efb176013791762d848cc734a2f69d28b7dac56abdfcd9cf`)
but remains unflashed so the bench diagnostics stay available.

On-device results from the bench image:

- Screen off, motion cadence: 101 successful timer wakes, 4.70 seconds spent
  within the sleep call during the five-second trial, zero errors.
- Inactive cadence: 55 successful timer wakes, 4.92 seconds within the sleep
  call, zero errors. The original foreground ran seven times in 6.6 seconds;
  all seven BMI270 reads succeeded.
- A second inactive trial reached 106 cumulative sleeps and 9.93 seconds
  cumulative sleep-call time, with 1,156/1,156 cumulative BMI270 reads
  successful; the bench M-hold then woke the original interactive view.
- A same-image source-faithful HGSS `back` and `put` after sleep each reached
  the original completion path with zero invalid packets (90/90 and 133/133
  valid receives). Independent 64 KiB EEPROM readback after `put` matched all
  10,430 course bytes, both status mirrors, and the source checksum.

The peer traces and sealed trial records are under
`stick/.build/trials/sleep-001-back/` and `sleep-002-put/`; the independent
readback and verdict are `stick/.build/trials/sleep-final-eeprom.bin` and
`sleep-final-verification.json`.

The individual GPIO wake on a physical M/R press and USB-disconnected battery
current have not yet been measured. The sleep-call duration includes entry
and exit overhead, so it is a residency indicator rather than an ammeter
reading. A whole-battery discharge result is still required for a runtime
estimate. No original step-count semantics were replaced by the BMI270's
hardware counter.

At M5Stack's published active-state current, 30 minutes of active use alone
consumes `36.69 mA × 0.5 h = 18.345 mAh` per day. Even granting zero drain
for the other 23.5 hours, the ideal 250 mAh battery lasts at most 13.6 days
under that assumption. This is a conditional bound from the manufacturer's
reference operating point, **not** a measurement of this port's actual
screen-on draw. It makes Nintendo's four-month daily-use claim an unsuitable
expectation for the stock StickS3 hardware unless the measured active current
is radically below M5Stack's published active state.
