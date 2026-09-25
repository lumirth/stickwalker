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

This patch reduces needless sensor and rail activity but does **not** make
the ESP32 sleep. The current firmware still has a foreground loop, 5 ms
button polling, and a 16 Hz motion path while walking. A full low-power
implementation needs measured state current, retained clock and source state,
prompt button wake, and motion samples with the timing and scale expected by
the original step algorithm. The BMI270 FIFO plus ESP32 Light-sleep is the
most direct candidate to test before attempting an L1 PMIC shutdown that
reboots the ESP32.

At M5Stack's published active-state current, 30 minutes of active use alone
consumes `36.69 mA × 0.5 h = 18.345 mAh` per day. Even granting zero drain
for the other 23.5 hours, the ideal 250 mAh battery lasts at most 13.6 days
under that assumption. This is a conditional bound from the manufacturer's
reference operating point, **not** a measurement of this port's actual
screen-on draw. It makes Nintendo's four-month daily-use claim an unsuitable
expectation for the stock StickS3 hardware unless the measured active current
is radically below M5Stack's published active state.
