# StickS3 debugging and power verification

Research date: 2026-09-26. Scope: current `m5stack:esp32:m5stack_sticks3` port, native USB Serial/JTAG, ESP-IDF 5.5 family. This report used manufacturer documentation and installed source; it performed no hardware interaction, build, firmware edit, Git operation, or paired-save access. **Guidance** below is a recommended next procedure. **Evidence** is either inspected source/configuration or explicitly supplied session observations. **Inference** is identified separately.

## Priorities for this bench

1. **Inspect the existing crash snapshot first.** The parent investigation found a nonempty dump in its pre-flash backup; verify checksum, embedded application ELF identity, and decode against the matching ELF before assigning it to today's failures. A surviving old dump is not an event timestamp. The current SDK already enables ELF/CRC32 flash core dumps, brownout detection, and stack-end watchpoints; inspect those facilities before adding another logger. [IDF core dumps](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/core_dump.html); installed configuration (local evidence: `bench/.tools/arduino-data/packages/m5stack/tools/esp32s3-libs/3.3.9/qio_opi/include/sdkconfig.h`).
2. **Keep USB sleep vetoed; make bench `Y` arm a battery-only trial.** Execute sleep only after physical USB disconnection and valid power-source observation; reconnect afterward to retrieve diagnostics. Native USB is not an independent observer of sleeping silicon. [IDF USB sleep limitations](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/usb-serial-jtag-console.html#sleep-mode-considerations).
3. **Capture exact errors outside the sampler.** Record the return code and calling stage, requested deadline/duration, button levels, elapsed time, and wake cause. Distinguish a normal rejected opportunity from a configuration failure. [IDF sleep API source](https://github.com/espressif/esp-idf/blob/v5.5/components/esp_hw_support/include/esp_sleep.h#L584-L594).
4. **Use built-in JTAG for an awake reproduction; repeat standalone for timing/watchdogs.** An attached debugger changes failure handling and can disable watchdog enforcement. [IDF watchdog/debugger behavior](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/wdts.html#jtag-watchdogs).
5. **Measure battery-path current before claiming savings or runtime.** Voltage and sleep counters are useful diagnostics, but do not measure current or energy. [M5PM1 register map, printed p. 7](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_EN.pdf); [Espressif current measurement guidance](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/current-consumption-measurement-modules.html).

## USB: use the supported workflow

**Guidance:** preserve the VBUS guard during USB diagnosis. IDF documents that manual light sleep gates APB/PHY clocks; the host can mark CDC erroneous/disconnected and fail to re-enumerate afterward because D+ stays asserted. Physical unplug/replug is the documented recovery. Manual entry has no USB safety rejection. `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION` protects automatic sleep only; it cannot protect this manual scheduler. [IDF 5.5 USB console](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/usb-serial-jtag-console.html).

**Inference:** the observed missing port followed by successful unplug/replug is consistent with that limitation; it does not establish a CPU crash. An `esptool run` recovery changes execution/reset state and is not passive evidence of the original fault. If download recovery is required, StickS3 documents its side-button download procedure; use the existing application-only restore procedure that preserves paired data. [M5Stack download mode](https://docs.m5stack.com/en/core/StickS3#download-mode).

Do not introduce USB PHY reset/register tricks without a separately reproduced need. First distinguish CDC FIFO/backpressure from host enumeration loss: the installed Arduino `HWCDC` has connection-event handling, bounded writes, and flush behavior. `Serial` truthiness describes CDC state, not battery power; logging and flush calls can delay execution. Keep serial output outside optical acquisition. [Arduino hardware CDC source](https://github.com/espressif/arduino-esp32/blob/3.3.9/cores/esp32/HWCDC.cpp); installed CDC implementation (local evidence: `bench/.tools/arduino-data/packages/m5stack/hardware/esp32/3.3.9/cores/esp32/HWCDC.cpp`).

## Battery-only sleep validation

**Guidance, applied to this port:** arm a finite trial over USB, unplug, wait for validated battery supply, run the unchanged scheduler, then reconnect. Stop the trial before reporting; retain its completed/incomplete state. Record start/end sample counts, successful sleep count, elapsed sleep time, rejection/error counts, wake-source counts, scheduler deadlines missed, and power/rail configuration. Treat a failed PMIC voltage read as unknown power state, not proof that VBUS is absent. No serial wait should be required for the battery run. [IDF sleep modes](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/sleep_modes.html); [current scheduler](../power_sleep.cpp).

**Evidence supplied by the parent:** approximately five seconds inactive produced 56 sleeps and five accelerometer reads; a visible 16 Hz trial produced 82 sleeps and 80 reads. These independently establish repeated successful sleep returns and the intended two sampling regimes. They do not establish battery current, exact asleep duty fraction, deadline fidelity, or long-term wake reliability. Four entry errors in an earlier trial caused four one-second awake periods; their unrecorded codes cannot now be reconstructed.

**Guidance:** IDF defines `ESP_ERR_SLEEP_REJECT` for a pending wake source and `ESP_ERR_SLEEP_TOO_SHORT_SLEEP_DURATION` for an interval consumed by entry overhead. Both alias ordinary `esp_err_t` values, so preserve the API stage alongside the symbolic/numeric code. Count those opportunities separately and return to scheduled work; reserve a lengthy reinitialization cooldown for a demonstrated configuration/driver failure. The current source already distinguishes these entry cases. [IDF sleep header](https://github.com/espressif/esp-idf/blob/v5.5/components/esp_hw_support/include/esp_sleep.h#L128-L131); [current distinction](../power_sleep.cpp).

## Debugger and optical timing constraints

**Guidance:** use Espressif OpenOCD with `board/esp32s3-builtin.cfg` and the exact firmware ELF while sleep is disabled. Hardware breakpoints at startup or after acquisition are appropriate for PMIC setup, task stacks, ownership, and post-capture state. For an observational session disable flash-breakpoint support (`set ESP_FLASH_SIZE 0` before loading the target configuration), avoid target reset commands unless reset is the test variable, and use hardware breakpoints explicitly. Software flash breakpoints modify code in flash. The chip offers two watchpoints, but this build reserves index 1 for stack-end detection. [IDF JTAG setup](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/jtag-debugging/index.html); [breakpoint/watchpoint constraints](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/jtag-debugging/tips-and-quirks.html).

**Inference from source:** never halt, single-step, print, add GPIO markers, change clock/optimization, or add tracing inside the GPIO5 gate/worker when qualifying optical reception. The gate uses cycle-count deadlines and direct pad control; halting necessarily changes its physical waveform. Use existing acquisition health counters/buffers and inspect them afterward. A debugger session can explain software state but cannot certify the unmodified real-time capture. [gate source](../ir_gate.cpp); [worker source](../ir_transport.cpp).

OpenOCD disables interrupt/task watchdog timers at breakpoints and does not restore them afterward. Repeat any watchdog or starvation test after a fresh standalone boot. An attached OCD-aware panic transfers control to GDB and bypasses normal console/core-dump handling. [IDF watchdogs](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/wdts.html#jtag-watchdogs); [IDF panic handler](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/fatal-errors.html#panic-handler).

Built-in USB JTAG loses the same sleep transport. External JTAG is not a drop-in solution here: GPIO39–42 overlap StickS3 LCD/IR wiring and require routing changes. A future separate UART observer on exposed GPIO43/44 is more practical for disconnected USB testing, after verifying wiring, voltage, and lack of back-power. Neither probe is currently available; retained diagnostics are the available method. [JTAG pins](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/jtag-debugging/tips-and-quirks.html#can-jtag-pins-be-used-for-other-purposes); [StickS3 schematic](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/K150_Stick_S3_PRJ_V0.6_20251111_2025_11_17_16_10_24.pdf).

## Reset, retention, and existing core dumps

**Guidance:** report `esp_reset_reason()` early, before PMIC initialization, with firmware identity and the prior trial record. This distinguishes USB/JTAG reset, software restart, panic, watchdog, deep-sleep wake, and brownout. Record a new boot identifier without overwriting an unreported prior record. [IDF reset API](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/misc_system_api.html#reset-reason).

`RTC_NOINIT_ATTR` is specifically intended to survive restart and deep-sleep cycles. Use a versioned record with magic, sequence/commit indicator and checksum; read a snapshot before clearing. **Inference:** retention after the observed USB CPU resets is good evidence for those resets, not a guarantee through PMIC shutdown, battery exhaustion, brownout, or every reset domain. It is not persistent paired-game storage. [IDF attribute definition](https://github.com/espressif/esp-idf/blob/v5.5/components/esp_common/include/esp_attr.h#L78-L86).

**Guidance:** decode the existing coredump partition (`0x7f0000`, `0x10000` in the selected 8 MB table) from the already saved snapshot first. Use official `esp-coredump`/GDB tools and validate the dump's integrity/application identity; keep the matching ELF available. Flash dumps survive loss of serial output. An empty/invalid dump does not rule out transport loss, power loss, a non-panicking hang, or a watchdog reset that never reached a successful dump write. Most external-RAM data is excluded. [IDF core dumps](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/core_dump.html); installed partition table (local evidence: `bench/.tools/arduino-data/packages/m5stack/hardware/esp32/3.3.9/tools/partitions/default_8MB.csv`).

## PMIC/I2C first-read failure

**Evidence:** this port uses M5Unified `m5::M5PM1_Class`, not the separate newer `M5PM1` library. Its first operation reads four ID bytes at register `0x00`; only after success does it disable I2C-idle sleep and the PMIC watchdog. The wrapper collapses the lower-level result to `bool`, and the installed M5GFX backend can classify NACK, timeout and arbitration loss under `connection_lost`. Retrying/reinitializing works around a failure but does not identify its mechanism. [installed PMIC source](https://github.com/m5stack/M5Unified/blob/0.2.21/src/utility/power/M5PM1_Class.cpp#L63); [wrapper](https://github.com/m5stack/M5Unified/blob/0.2.21/src/utility/I2C_Class.cpp#L76); [backend](https://github.com/m5stack/M5GFX/blob/0.2.29/src/lgfx/v1/platforms/esp32/common.cpp#L1671).

**Guidance:** keep bounded recovery, but record attempt, boot/reset reason, address/register, bus frequency, read length, returned bytes only on success, elapsed transfer time, failed stage, and lower-level error/raw interrupt flags before recovery clears them. Sample SDA/SCL both before the transaction and on failure. HIGH/HIGH afterward says only that both lines were released at that instant; it cannot establish whether an address was ACKed or whether START/STOP timing was correct. A logic-analyzer capture of the failed first transaction is the next physical discriminator if available. IDF explicitly recommends inspecting ACK/NACK on SDA/SCL for failed probes. [IDF I2C error/probe guidance](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/peripherals/i2c.html#i2c-master-probe).

M5PM1 supports idle sleep at `0x09` and an I2C START wake mechanism; the manufacturer's newer driver already provides wake support/retries. **Inference:** this is an existing mechanism worth checking before inventing bus pulses, but disabled idle sleep in the previous running configuration would weaken that hypothesis. Read back configuration after successful initialization rather than assuming it. Do not install a second IDF I2C master driver on M5Unified's owned bus to gain error codes. [M5PM1 datasheet, I2C configuration and idle sleep](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_EN.pdf); [manufacturer wake implementation](https://github.com/m5stack/M5PM1/blob/main/src/M5PM1_i2c_compat.h#L215-L224); [manufacturer bus-sharing guidance](https://github.com/m5stack/M5PM1/blob/main/README_FUNCTION_EN.md#sharing-i2c-with-m5unified--m5gfx).

## Whole-device power and brownout verification

**Guidance:** measure at the battery supply path, including PMIC, conversions, IMU, display/audio rails, IR boost, and retained RAM; use a low-burden current/energy instrument that resolves sleep current and active peaks. Espressif recommends instruments such as Joulescope or Nordic PPK2 because ordinary ammeters can miss dynamic range changes and cause supply droop. Its module-only method deliberately excludes board loads; our whole-device objective must include them. Capture current and voltage together across complete idle, visible 16 Hz, audio and IR cycles, then integrate charge/energy over representative use. [Espressif measurement method](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/current-consumption-measurement-modules.html).

**Inference from schematic:** the accessible BAT pin permits voltage observation, but is parallel to an internally connected battery; putting an ammeter in that header lead does not measure all battery current. Whole-device battery-current measurement needs a defined series battery path or a correctly isolated substitute supply. A 5 V USB meter measures that input, including charging/power-path effects, not battery-mode consumption. Verify the board's permitted input route and `EXT_5V_EN` state before designing the fixture. [StickS3 power schematic](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/K150_Stick_S3_PRJ_V0.6_20251111_2025_11_17_16_10_24.pdf); [M5Stack supply-routing notice](https://docs.m5stack.com/en/core/StickS3#note).

M5PM1 exposes voltage registers and generic ADC channels, not a documented supply-current/coulomb counter. Battery-voltage decline, voltage-derived percentages, component datasheet currents, and M5Stack's published power-state figures cannot establish this port's average battery current. PMIC L1–L3B switches are independently sourced; verify actual rail states rather than equating CPU sleep with peripheral shutdown. [M5PM1 register map](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_EN.pdf); [M5Stack power-level design](https://docs.m5stack.com/en/arduino/m5sticks3/m5pm1#low-power-configuration).

Battery audio peaks are a concrete separate concern: M5Stack recommends speaker volume below 75% on battery to avoid reboots. The port's native volume numbers alone do not establish delivered amplitude/current because sample data, master gain and magnification also matter. Record reset reason during representative battery audio and, when an instrument is available, observe battery and 3V3 rail minima during the peak. Do not disable brownout detection to make the symptom disappear. A fast droop can truncate the panic message; missing serial output is not proof that brownout did not occur. [M5Stack speaker notice](https://docs.m5stack.com/en/core/StickS3#note); [IDF brownout behavior](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/fatal-errors.html#brownout).

## What the available setup can establish

| Method | Supported conclusion | Remaining limit |
| --- | --- | --- |
| USB connected, sleep vetoed | Awake code paths, serial/control health, PMIC register/rail configuration | Different power source and no sleep qualification |
| Built-in JTAG, sleep vetoed | Software state, stacks, startup/I2C failure location | Halts distort timing; watchdog/panic behavior changes |
| Battery run + validated retained record | Successful sleep/wake counts, sampling regime, entry errors, reset/trial completion | No measured current/energy; retention depends on reset/power domain |
| Validated existing flash core dump + matching ELF | State of the represented panic | May predate current trials; does not describe every failure class |
| PMIC voltage telemetry | Supply voltage at sampled times | No supply-current measurement; short dips can be missed |
| Future battery-path current/voltage capture | Whole-device average/peaks/energy and rail-droop correlation | Requires an appropriate instrument and verified series fixture |

The paired Pokémon is a task constraint throughout: use the existing save-preserving restoration procedure, keep crash diagnostics separate from game storage, and never replace the paired-device firmware with a generic low-power example just to obtain a benchmark.

## Applied debugger configuration finding

The subsequent production probe could examine CPU0 while CPU1 returned an
invalid OCD ID. The dual-core configuration's memory values were inconsistent
with the matching ELF; do not interpret them as application state. The installed
Espressif target scripts explicitly support `set ESP_ONLYCPU 1` before loading
`board/esp32s3-builtin.cfg`. With that configuration, a brief halted read returned
the expected descriptor magic, runtime ready=1 and USB veto=1. It was followed
by a fresh standalone boot. This does not identify why CPU1 was unavailable or
qualify receiver timing under a debugger. Preserve the failed probe logs too.
Installed target configuration (local evidence: `bench/.tools/arduino-data/packages/m5stack/tools/openocd-esp32/v0.12.0-esp32-20251215/share/openocd/scripts/target/esp32s3.cfg`).

## Verify the post-flash handoff

A successful flash digest and `esptool run` exit do not prove a live production
application. In the wake follow-up, the initial CPU0 snapshot lacked a valid
application descriptor/state. Opening the existing Link with DTR/RTS false
before open produced a fresh USB-UART boot, after which CPU0 reads confirmed
the exact descriptor, ready=1 and advancing native RTC seconds. Treat that
open as a reset-capable recovery action, not passive observation. After the
final control-line handoff, verify liveness and leave that state running;
do not finish with another unverified reset. See the
[wake follow-up](wake-transition-and-runtime-estimate-2026-09-26.md).
