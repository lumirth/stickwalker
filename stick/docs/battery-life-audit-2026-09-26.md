# Stickwalker battery-life audit — 2026-09-26

## Conclusion

The current low-power implementation is incomplete. It puts the ESP32 into
Light-sleep between foreground work, but leaves the display/audio peripheral
supplies enabled. Turning off the backlight does not put the LCD controller to
sleep. Finishing a sound turns off the speaker amplifier, but leaves the codec
configured for playback and retains the I2S output. Additional wake overhead and
production sleep blockers compound those persistent loads.

These defects provide a credible software explanation for a battery that fails
to last a day, even if the processor usually sleeps. Confidence is high in the
identified lifecycle defects; their individual battery-current contributions
are inferred from component specifications, rather than measured on this port.
No device connection, firmware flash, reset, or saved Pokémon access was used
for this assessment.

## Audited implementation

- Port source: `c2e7f76`, branch `stick-s3`; clean before the audit artifacts.
- Latest production app: `sound-amp-hold-production-app.bin`, SHA-256
  `20afaff5c8d0740eacffdd2b896536324fd4aa204fca83655e495374dc041f5b`.
  The current `stick/.build/output/PwStick.ino.bin` matches that artifact.
- Build target: `m5stack:esp32:m5stack_sticks3`, local Arduino platform 3.3.9,
  ESP32-S3 SDK configuration `qio_opi`.
- Local M5Unified `Speaker_Class.cpp` SHA-256:
  `578c98b9e8ed2efa3382ef31cdd68319e9719028318ffa4f6012d50a09c2e255`.
- Local M5Unified `BMI270_Class.cpp` SHA-256:
  `8f79e013088e20ca65b45229677496de29d9c86f06b1d43aa70d4ca4889bbf36`.

The local library implementations matter: conclusions below were checked
against them, rather than assuming the generic behavior of `stop()` or a
low-power API.

## Persistent loads

### 1. LCD power-save stops only the backlight — major confirmed defect

The original `DisplayEnterPowerSave()` stops the NT7508 oscillator and power
circuits. The port translates its command into `screen_enabled=false`, then
`StickDisplayPanelSetBacklight(0)` only calls `setBrightness(0)`.

The ST7789 is initialized with `SLPOUT` and `DISPON`; the port never sends
`SLPIN`, calls `setSleep()`, or removes LCD power. Its oscillator, panel scanning,
and internal power circuits therefore retain their active configuration while
the screen looks off.

The actual controller is ST7789P3, a TFT LCD. Black pixels do not remove its
scanning or backlight load as they would on an OLED. Sitronix specifies about
6 mA main-supply current for normal mode with a black image, versus 20 µA in
Sleep-in, plus about 5 µA I/O current in either case. Conditions are 60 Hz,
25 °C, VDD 2.75 V and VDDI 1.8 V; the port's panel configuration and 3.3 V supply
are different, so these are scale estimates. `SLPIN` stops the converter,
oscillator, and scanning while retaining display RAM. See pp. 34 and 143 of the
[ST7789P3 datasheet](https://files.waveshare.com/wiki/ESP32-S3-GEEK/ST7789P3.pdf).

Sources: [display bus](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/display_bus.cpp:23),
[panel adapter](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/display_panel.cpp:52),
[original display shutdown](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/src/application/pw_nt7508.c:430),
[ST7789 initialization](/Users/lu/Desktop/stick-s3-capability-investigation/bench/.tools/arduino-user/libraries/M5GFX/src/lgfx/v1/panel/Panel_ST7789.hpp:79),
[LCD sleep method](/Users/lu/Desktop/stick-s3-capability-investigation/bench/.tools/arduino-user/libraries/M5GFX/src/lgfx/v1/panel/Panel_LCD.cpp:101).

### 2. Shared peripheral power stays on — major confirmed defect

`StickBoardBegin()` enables PMIC GPIO2 once. There is no later GPIO2 disable
anywhere in the port. This leaves L3B powered through CPU sleep.

The schematic distinguishes LCD `3V3_L3B`, supplied from the ESP supply through
a load switch, from audio/microphone `3V3_L3B_AU`, supplied through its own LDO.
GPIO2 controls both. The LCD supply and audio supply thus survive independently
of whether the ESP's clocks are running. Disabling the amplifier's GPIO3 does
not disable the codec or microphone supplies.

This rail defect is the enabling condition for the LCD, codec, and microphone
loads below; it is not an additional current to add on top of those loads.
[M5Stack power switching documentation](https://docs.m5stack.com/en/arduino/m5sticks3/m5pm1),
[StickS3 schematic, sheets 2–3](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/K150_Stick_S3_PRJ_V0.6_20251111_2025_11_17_16_10_24.pdf).

Source: [peripheral rail initialization](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/board_hal.cpp:59).

### 3. The codec is never suspended after a sound — major confirmed defect

After the first score, `speaker_power(true)` configures the ES8311. Its false
path writes only PMIC GPIO3 and immediately returns. Neither normal score
completion, the 500 ms amplifier hold expiry, nor IR quiescence writes codec
shutdown registers. `codec_on=false` records amplifier state, not actual codec
power state; previous diagnostics using that name could misleadingly appear
to show complete audio shutdown.

The codec's published normal-operation figure is 8 mA at AVDD 3.3 V and
DVDD/PVDD 1.8 V. Our supply and DAC-only use differ, so assigning exactly 8 mA
to this port would be unjustified. It nevertheless establishes that an active
codec is a milliamp-scale concern. The audio LDO means output current is
approximately drawn from the battery input as current, with regulator overhead.
[ES8311 datasheet, p. 9](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/products/atom/Atomic%20Echo%20Base/ES8311.pdf).

Espressif's codec driver performs a multi-register suspend sequence separately
from disabling the amplifier; the port does neither codec suspend nor rail
removal. [Espressif ES8311 driver](https://github.com/espressif/esp-adf/blob/release/v2.x/components/esp_codec_dev/device/es8311/es8311.c).

Sources: [codec/amp power](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/sound_bridge.cpp:52),
[output stop](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/sound_bridge.cpp:83),
[score completion](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/sound_bridge.cpp:147),
[IR quiescence and hold expiry](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/sound_bridge.cpp:227).

### 4. Silent I2S output survives normal score completion — confirmed defect

`M5.Speaker.stop()` stops queued sound; it does not end the driver. The normal
idle path never calls `M5.Speaker.end()`. That call exists only on IR entry.

In the local speaker implementation, the no-data path drains zeros and blocks
the worker on a notification. Its idle `_i2s_stop()` is inside the built-in-DAC
branch, which does not apply to ESP32-S3 or this configuration. The I2S channel
therefore remains enabled after playback. The worker is blocked, not spinning;
this is a clocks/DMA/peripheral lifecycle problem, not evidence that a busy
audio thread prevents every manual Light-sleep call.

At the configured 22,050 frames/s and 64-frame DMA blocks, the nominal block
completion rate is about 345/s while the I2S clocks run. Manual Light-sleep
gates the ESP's digital clocks, so this is not a claim of 345 wakeups/s during
sleep. It adds unnecessary work and keeps the codec clocked during awake
intervals. IR entry closes I2S but still does not suspend the codec.

Sources: [audio setup](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/sound_bridge.cpp:94),
[normal completion](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/sound_bridge.cpp:147),
[IR teardown](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/sound_bridge.cpp:227),
local speaker [idle path](/Users/lu/Desktop/stick-s3-capability-investigation/bench/.tools/arduino-user/libraries/M5Unified/src/utility/Speaker_Class.cpp:599),
[task exit](/Users/lu/Desktop/stick-s3-capability-investigation/bench/.tools/arduino-user/libraries/M5Unified/src/utility/Speaker_Class.cpp:997),
[end method](/Users/lu/Desktop/stick-s3-capability-investigation/bench/.tools/arduino-user/libraries/M5Unified/src/utility/Speaker_Class.cpp:1069),
[stop method](/Users/lu/Desktop/stick-s3-capability-investigation/bench/.tools/arduino-user/libraries/M5Unified/src/utility/Speaker_Class.cpp:1105).
[Espressif I2S transport and power management](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/peripherals/i2s.html).

### 5. The unused microphone is continuously powered — small confirmed load

The analog MSM381A3729H9BPC microphone is directly supplied from the audio
rail. Avoiding `M5.Mic.begin()` stops software recording; it cannot turn off the
physical microphone. The mirrored manufacturer datasheet lists about 150 µA
typical. This is much smaller than the LCD/codec loads but is unnecessary for
Pokéwalker functionality.
[Microphone datasheet](https://www.ibtikar.io/Robotics/MSM381A3729H9BPC.pdf),
[StickS3 schematic, sheet 3](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/K150_Stick_S3_PRJ_V0.6_20251111_2025_11_17_16_10_24.pdf).

### 6. Accelerometer runs continuously at its startup settings — smaller contributor

M5Unified disables advanced power saving with `PWR_CONF=0x00`; the port then
disables gyro, auxiliary sensing, and temperature using `PWR_CTRL=0x04`.
It never changes `ACC_CONF`, whose reset setting is `0xA8`: normal/performance
filtering at 100 Hz. The host consumes only 16 samples/s during motion and one
sample/s while inactive. Original accelerometer control writes in the port's
BMA150 seam only update software variables; they do not change the BMI270.

Bosch specifies about 210 µA for accelerometer-only normal operation and 10 µA
for its 25 Hz low-power example, at VDD 1.8 V. This cannot explain a sub-day
250 mAh lifetime alone. It does become important after fixing milliamp loads.
Rate/filter changes need to preserve the original motion algorithm's scale,
timing, and bandwidth rather than substituting the hardware step counter.
[BMI270 datasheet, pp. 11, 26, 100](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf).

Sources: [sensor initialization](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/board_hal.cpp:50),
[BMA150 seam](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/accel_bridge.cpp:38),
local BMI270 [startup](/Users/lu/Desktop/stick-s3-capability-investigation/bench/.tools/arduino-user/libraries/M5Unified/src/utility/imu/BMI270_Class.cpp:48)
and [power configuration](/Users/lu/Desktop/stick-s3-capability-investigation/bench/.tools/arduino-user/libraries/M5Unified/src/utility/imu/BMI270_Class.cpp:89).

## CPU and wake overhead

### 7. Stationary background still wakes at least roughly ten times/s

The inactive native foreground correctly runs at one sample/s, but
`StickSleepUntil()` caps every sleep at 100 ms so it can poll the PMIC side
button. Each timer wake forces an input poll, including an I2C transaction.
This defeats much of the stationary one-second interval. Motion additionally
requires 16 Hz foreground sampling, with RTC/quarter-second deadlines.

The previous five-second hardware trial reported 101 successful sleep calls
and 4.70 seconds inside them at motion cadence, and 55 calls/4.92 seconds while
inactive. Those are useful historical scheduler checks, but include sleep
entry/exit time and predate the final audio changes. They are neither current
measurements nor proof that the peripheral supplies were off.

Each wake returns to 240 MHz. There is no frequency reduction outside IR.
The SDK lacks `CONFIG_PM_ENABLE` and tickless idle; `delay(1)` yields to FreeRTOS
but does not automatically place the processor in a low-power state. Explicit
manual Light-sleep still works with that configuration. If it is vetoed,
the program spends the day running its normal 1 ms loop and 5 ms input polls.

Sources: [sample cadence](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/board_runtime.cpp:54),
[CPU setup](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/board_runtime.cpp:140),
[input polling](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/board_runtime.cpp:503),
[sleep policy](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/board_runtime.cpp:597),
[sleep duration cap](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/power_sleep.cpp:45),
[SDK configuration](/Users/lu/Desktop/stick-s3-capability-investigation/bench/.tools/arduino-data/packages/m5stack/tools/esp32s3-libs/3.3.9/qio_opi/include/sdkconfig.h:821).

### 8. More retained state than necessary — smaller remaining opportunities

- RTC peripherals are explicitly forced on to maintain internal button
  pull-ups. The schematic already supplies 10 kΩ external pull-ups to both
  buttons. EXT1 can operate with RTC peripherals off, so this extra retained
  domain can potentially be removed with correct wake-pad handling.
- Flash and 8 MiB PSRAM remain retained during Light-sleep. The SDK already
  enables both leakage workarounds. This is a residual baseline, not evidence
  of missing all memory power management. Simply removing the framebuffer
  allocation would not stop SDK PSRAM initialization or retention.
- SPI and backlight PWM resources remain initialized. Zero PWM duty really
  is zero in the local `Light_PWM` implementation, so its brightness offset
  does not secretly leave the backlight glowing. The timers and peripheral
  resources still merit cleanup while the display is off.
- After the first IR session, the TX RMT channel remains enabled forever.
  The sampler worker is normally deleted and the IR 5 V rail is turned off,
  but no `rmt_disable()` exists. This leaves a resource/clock lifecycle
  omission. With automatic PM enabled later, it would also retain the driver's
  PM lock. It is not established as a blocker of today's manual sleep calls.
- The local PMIC library explicitly disables I2C-idle sleep in `begin()`;
  the port never restores it or uses its retained-supply sleep mode.
  Frequent button reads would defeat an idle timeout anyway. Its own MCU,
  supply conversion losses, and quiescent currents add a smaller baseline;
  no port-specific value has been assigned to them.

[ESP-IDF retained domains and flash behavior](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/sleep_modes.html),
[RMT power management](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/peripherals/rmt.html).
Sources: [RTC retention](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/power_sleep.cpp:35),
[TX resource initialization](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/ir_tx.cpp:25),
[RX cleanup](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/ir_transport.cpp:382),
[PWM duty implementation](/Users/lu/Desktop/stick-s3-capability-investigation/bench/.tools/arduino-user/libraries/M5GFX/src/lgfx/v1/platforms/esp32/Light_PWM.cpp:120),
[PMIC idle-sleep disable](/Users/lu/Desktop/stick-s3-capability-investigation/bench/.tools/arduino-user/libraries/M5Unified/src/utility/power/M5PM1_Class.cpp:66).

## Conditions that can remove CPU sleep entirely

### 9. Unread USB input permanently vetoes sleep in production — confirmed edge bug

`Serial.begin()` runs in production, but the receive/command loop is compiled
only for the bench build. The production sleep condition nevertheless requires
`!Serial.available()`. One received byte can stay in the RX queue forever,
including after unplugging USB, and prevent every subsequent sleep. Arduino's
weak serial event hook does not consume it; the port implements no handler.

This is a plausible additional failure mode after using a host serial tool,
but there is no evidence that it occurred on this particular battery run.

Sources: [serial initialization](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/board_runtime.cpp:141),
[bench-only receive loop](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/board_runtime.cpp:210),
[production sleep veto](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/board_runtime.cpp:597),
[USB queued-byte count](/Users/lu/Desktop/stick-s3-capability-investigation/bench/.tools/arduino-data/packages/m5stack/hardware/esp32/3.3.9/cores/esp32/HWCDC.cpp:683),
[weak serial hook](/Users/lu/Desktop/stick-s3-capability-investigation/bench/.tools/arduino-data/packages/m5stack/hardware/esp32/3.3.9/cores/esp32/HardwareSerial.cpp:89).

### 10. Sleep failure is silently tolerated — confirmed robustness defect

Setup ignores `StickSleepBegin()`'s result and marks the runtime ready anyway.
If setup fails, `ready` in the sleep driver stays false forever. Runtime sleep
errors also fall back to `delay(1)`. Counters exist, but their readout is excluded
from production. A dark screen therefore provides no evidence that sleep
actually works. The code needs recovery and a usable failure indication.

This is an identified failure path, not a claim that initialization failed on
the user's device. Sources: [ignored setup result](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/board_runtime.cpp:154),
[runtime fallback](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/board_runtime.cpp:617),
[driver initialization](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/power_sleep.cpp:22),
[driver error path](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/power_sleep.cpp:61).

### 11. Settings, held inputs, sound, and Connect keep the CPU awake

- The Stick Settings overlay has no idle close or power-save timeout. It
  explicitly lights the panel and vetoes sleep even if the native game has
  become inactive beneath it. Leaving it open is an unlimited active state.
- A held front/side button, a queued native scan, or PMIC re-arm in progress
  also vetoes sleep. Ordinary re-arm settles after 50 ms of quiet release;
  there is no evidence that released controls stay stuck indefinitely. A
  button physically pressed in a pocket can retain active CPU operation.
- Native sound and IR foreground owners are excluded from the sleep branch.
  This is necessary while servicing their present timing design, but it means
  a sound/connection screen is not low-power background operation.
- USB/VBUS deliberately suppresses sleep even with the display off. That
  normally happens on external power and is not itself battery-only drain;
  unplugging is detected within the background VBUS polling path.

Sources: [overlay rendering](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/board_runtime.cpp:100),
[IR foreground](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/board_runtime.cpp:467),
[overlay controls](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/board_runtime.cpp:519),
[sound and sleep policy](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/board_runtime.cpp:579),
[PMIC re-arm](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/input_bridge.cpp:109),
[gesture wake condition](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/input_bridge.cpp:243).

## Checked contributors that do not explain the persistent drain

- **Green LED:** current setup explicitly disables it. It is not the remaining
  continuous drain on this build.
- **Gyro/temperature:** disabled by `PWR_CTRL=0x04`.
- **Wi-Fi/Bluetooth:** no startup calls in the port or its Arduino startup
  path. Having the libraries enabled in the SDK is not equivalent to running
  their radios.
- **Optical sampler:** created only during an IR session and normally deleted
  by `StickIrStop()`. It is not continuously sampling in background.
- **External 5 V boost:** disabled after startup and normal IR completion.
  Cleanup is missing verification of the PMIC write result, but there is no
  evidence of a stuck-on boost. If worker shutdown exceeds its 500 ms limit,
  the handle is cleared without deleting a not-yet-finished task; this abnormal
  cleanup path merits hardening, not attribution as the normal idle load.
- **EEPROM access:** normal reads use the in-RAM image. Writes use a journal
  and skip unchanged spans; compaction is occasional. Hourly save/diary writes,
  settings, events, and games cost bursts of energy, but there is no per-loop
  whole-image write or flash write for every step. `StoreTotalSteps()` updates
  RAM. The retained memory supply is the continuous cost.
- **Battery percentage:** the linear 3.3–4.1 V estimate is not a fuel gauge.
  Showing 100% at 4.1 V does not prove a completed 4.2 V charge, and load/charge
  recovery changes the displayed estimate. This can make runtime estimates
  misleading; it is not a current drain. The charging circuit sets nominal
  charge current in hardware, about 190 mA in the schematic; the port contains
  no battery-charge-disable call. Battery wear or undercharge cannot be
  determined from software, but is unnecessary to explain these defects.

## Battery arithmetic and confidence

For a nominal 250 mAh battery, ideal average-current budgets are:

| Runtime | Average battery current |
| --- | ---: |
| 8 hours | 31.25 mA |
| 12 hours | 20.83 mA |
| 18 hours | 13.89 mA |
| 24 hours | 10.42 mA |
| 7 days | 1.49 mA |

Thus it takes only about 10 mA of persistent average drain to miss one day.
LCD normal operation plus a codec left enabled are already in this scale;
CPU wake work and the smaller loads add to it. An illustrative 14 mA battery
load lasts 17.9 hours ideally. This is a consistency check, not a prediction
obtained by adding datasheet currents from different rails/test conditions.

If CPU sleep is entirely blocked, M5Stack's published L3A reference of
36.69 mA corresponds to only 6.8 hours before adding other loads. Its published
L2 reference of 102.4 µA is not this implementation: peripherals and repeated
wake work prevent using that number as a Stickwalker standby estimate.
[Capacity and reference states](https://docs.m5stack.com/en/core/StickS3).

The largest software explanation is **persistent peripheral power**, compounded
by **incomplete audio teardown**, then **wake overhead or a complete sleep
veto**. Smaller sensor/retention loads matter for a multi-day goal, but are not
credible primary explanations for the present sub-day behavior.

## Reproduction without the device

Run `python3 stick/audits/idle_power_trace.py` from the port root. The
[probe source](/Users/lu/Desktop/stick-s3-capability-investigation/pw-stick-s3/stick/audits/idle_power_trace.py)
compiles the
actual production runtime, virtual display bus, panel adapter, sound adapter,
and sleep scheduler unchanged against small hardware/foreground stubs.
It runs in about one second, reads no saves, and leaves its temporary binary
outside the repository. Exit 1 indicates identified power-policy violations.

The audit output on this revision shows:

| Path | Lifecycle result |
| --- | --- |
| Quiet screen-off inactive control | 12 stub sleep completions in 1.2 s; backlight zero; no LCD sleep command |
| Same path, one serial byte added | Zero sleeps; byte still unread; native foreground continues |
| Same path, sleep setup error injected | Zero sleeps; native foreground continues |
| Score ends and 600 ms pass | Amplifier off; zero speaker `end()` calls; zero codec shutdown writes |

GPIO2 remaining on is also independently established by the board source;
the stub initializes that observed board state. This harness proves control
flow and shutdown omissions, **not current, real sleep residency, motion
behavior, or an actual battery lifetime**. Its numeric sleep counts use a
deterministic stub clock and must not be presented as new hardware results.

## Implementation consequences

1. Give display and audio explicit lifecycle ownership of L3B. When neither
   needs it, stop output, park SPI/I2S pins against back-powering, and gate GPIO2.
   Restore supplies and hardware configuration on demand. Native display RAM
   already lives in the ESP and can redraw after LCD reinitialization.
2. Put the LCD into real Sleep-in whenever its backlight becomes inactive,
   including when the rail must remain on for audio. Use `setSleep()`, not
   `setPowerSave()`: M5GFX's latter method only selects LCD idle color mode.
3. End I2S and suspend the codec when the 500 ms amplifier hold expires.
   Preserve the verified 65 ms amplifier startup behavior and source score
   timing; tear down once, then reinitialize on the next score.
4. Remove production serial input from the sleep veto, or explicitly consume
   unused input. Handle sleep initialization and runtime errors with recovery
   and production-visible health, rather than silent permanent full-power
   fallback.
5. Close or sleep the Stick Settings overlay after inactivity. Make its wake
   and close transitions obey the same display power lifecycle as the game.
6. Replace the 100 ms PMIC poll wake with a correctly masked/cleared IRQ while
   keeping the green LED off; then remove unnecessary RTC retention. The prior
   failed IRQ trial established a routing/status issue, not that IRQ wake is
   impossible.
7. Disable idle RMT/PWM resources. Reduce awake CPU frequency outside optical
   sampling, restoring 240 MHz before receiver setup. Keep the proven GPIO5
   machine waveform and source protocol timing unchanged.
8. Configure BMI270 low-power sensing and consider FIFO batching to reduce
   ESP wakes while retaining correctly timed input to the original algorithm.
   This follows the major shutdown fixes; it is not a prerequisite for them.

The assessment is complete. These implementation changes have not been applied
to production in this audit. Exact current attribution and final lifetime
confirmation are subsequent checks, not prerequisites for identifying or
correcting the demonstrated software defects.
