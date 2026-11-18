# Run Stickwalker end to end

Build the firmware and extract the resident artwork using the [build instructions](../docs/building.md). For IR lifecycle and timing changes, run `connections.py` below;
`journeys.py` covers the broader gameplay and power flows. Use the ESP32-S3 mask ROM ELF from Espressif's
`esp-rom-elfs` package, a paired empty EEPROM, a retail EEPROM containing a
Pokémon, and the local HGSS protocol peer (`bench/tests/hgss_driver.c` in IR Lab).

```sh
python3 e2e/journeys.py \
  /path/to/esp32s3_rev0_rom.elf build/firmware \
  /path/to/registered-empty.eeprom /path/to/course.eeprom \
  /path/to/hgss-dataset-reference
```

Add `--pair BEFORE TRACE PREDICTED` to run a recorded pairing exchange.
`BEFORE` is the original EEPROM and `PREDICTED` is the image reconstructed
from the captured page writes. Use `--output DIRECTORY` to look at the
rendered screens or the exchanged packets. Otherwise the runner uses a
temporary directory. `--timing maximum` selects the datasheet's -40..85°C maximum
program and erase times; the default uses its typical values. Program time depends
on the command's byte count. The runner also executes the mask ROM's 100 ms delay
at 10, 40, 80, 160, and 240 MHz to check CPU time against device time.

Run `python3 e2e/storage.py` for interrupted NOR writes and erases, bank replacement,
import retirement, RAM rollback, and full-image export checks. This compiles `main/save.c` with a
flash model and address/undefined-behavior sanitizers; it requires a C compiler
and zlib.

`python3 e2e/storage.py && cargo test --locked -p sw-host` adds the public firmware migration readers and
the host's storage/protocol checks. The `management` journey runs USB requests
through the emulated controller into the compiled firmware:

```sh
e2e/run /path/to/esp32s3_rev0_rom.elf build/firmware \
  /path/to/registered.eeprom management build/desktop-management
```

It checks coherent EEPROM export, palette persistence across reboot,
malformed-frame rejection, rollback and game resumption after abandoned transfers,
live import from save recovery, reset, screen wake on palette edits, and export after
connecting during a pocket walk. Native serial drivers, dialogs,
signing, and self-update still need the actual desktop platform.

The journeys check distinct identities on fresh boards and preserved identity on import.
They cover physical button presses and menu choices, a pocket walk,
Pokémon send and return, interrupted saves, USB insertion during a transfer,
USB removal on a depleted battery, bounded discovery under repeated optical noise,
screen-wake audio, bounded silent amplifier hold, and muted input, Front/Side wake and ignored Power taps during light sleep,
USB insertion before enumeration while already asleep, depleted-battery PM1 shutdown
and USB power-on,
and USB repair of corrupt startup software. Recovery is driven by a
continuous Power hold, the power chip's watchdog, or the USB control lines.
Recovery runs boot back into the game. When the firmware is corrupt, the
runner writes its replacement through the mask ROM. The board model uses the
[StickS3 schematic](https://docs.m5stack.com/en/core/StickS3), the
[M5PM1 manual](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_EN.pdf),
and the [flash timing table](https://download.gigadevice.com/Datasheet/DS-00484-GD25Q64E-Rev1.6.pdf).

PM1 shutdown stops the CPU until a Power or external-power event; it is not
modeled as ESP32 deep sleep. SYS_CMD restart/download and the sticky download
lock follow the published PM1 behavior, including rejection of `0xA3` when
locked. The register's GPIO direction applies to ordinary GPIO mode; the manual
does not require that direction bit for an IRQ-multiplexed output. The model
checks direction and mux for the LCD and amplifier supplies.
Every journey rejects button-feedback LED flashes, explicit watchdog disable,
and sleeping with USB enumerated. Idle cable-power detection is bounded by the
30-second housekeeping interval; a face-button wake checks it immediately. The watchdog journey stalls the actual CPU
three times and requires a playable restart each time, followed by USB ROM
recovery. These checks do not establish hardware current draw or immunity to
arbitrary corruption of the bootloader, app, or power-chip firmware.
USB removal during a healthy optical exchange leaves the battery supplying the
emitter. The transfer must complete and persist before the post-link battery
check shuts down a depleted walker; USB power-on must retain the accepted save.

The LCD starts with uninitialized GRAM and exposes the fixed physical glass
aperture through the controller's address transform. LCMCTRL and MADCTL jointly
select RGB/BGR output; the palette journey checks Amber in both rotations. SPI3
DMA completion includes the transfer time derived from its clock registers, so
LCD redraws compete with score deadlines. The motion journey adds
stationary sensor noise; the speaker observes PCM after codec setup and amplifier
settling, including a failed first codec write. Rapid Confirm/Back sequences
check pitched PCM duration and the retail score's silent separators, both on the
first interaction and after the amplifier cools. Codec access also requires its
shared LCD rail to have settled. Settings is closed at different
game-tick phases to check that its closing press cannot activate the game.

The gameplay journey uses the supplied retail course's nonzero encounter and
item thresholds, plays Radar and Dowsing, captures a Pokémon, checks inventory
after reboot, and returns the expedition to HGSS. Long-history fixtures supply
saved step totals; the separate pocket journey checks steps from sensor samples.
The pairing, send, return and gameplay checks compare all 65,536 EEPROM bytes
between live RAM and decoded flash.
Ordinary optical transfers must also finish without a CPU reboot; a watchdog
restart is not accepted as leaving the Connecting screen.

The peer journey runs two firmware instances with distinct identities. Each RMT
pulse reaches the other instance's photodiode model. The VMs advance in 128-cycle
slices with a 256-cycle channel delay to preserve causality. It exercises an
initial collision, the 16-bit gift-score calculation, same-day repeat suppression,
and midnight's step-history and peer reset.

`STICK_IR_PATH=wide-pad` selects a 35 pF diode node, 60 kΩ pull-down, 10 µA
signal and +0.87% peer clock offset. `bright` selects a 500 µA signal with varying
ambient light and -0.87% clock offset. These are circuit-envelope tests, not
measurements of the particular Stick on your desk. Device testing still checks
the actual panel, sensor, speaker, photodiode and cartridge together. The simulator
does not model Touch4/Touch5 capacitance classification; the bounded probe's
fallback and the valid-peer icon run here, but wiring diagnoses need device checks.

Ordinary `Rig::press` lasts 200 ms; explicit rapid-input tests retain shorter
presses. Pocket walking checks zero PM1 button reads and bounded total PM1 transactions,
alongside step credit and sleep residency. Storage checks that `save_begin_transfer()`
performs no flash operations, and that a transient program failure preserves
RAM and can be retried into the alternate bank. The existing transfer scenarios
also cover wide/slow RC paths and bright light, rather than adding a new boot
for every optical parameter. Use a single `e2e/run ... JOURNEY OUTPUT` while
iterating, then run the complete journeys serially for release verification.
The stationary cases observe four complete pedometer FFT windows after settling
at each pose, rather than repeating the same deterministic noise for a minute.
Sleep transitions have their own journeys; shortening this check does not shorten
Nintendo's timers in the firmware.
The pairing journey no longer waits 25 seconds to cross the removed boot-health
counter's deadline. It starts from the same visible boot-ready condition as the
other transfers.

The journey runner also replays physical quiet and split-pulse GPIO5 captures through the C
decoder once before simulator boots. Idle, concurrent CPU0-memory, and failed-transfer
captures check quiet-line rejection, including a quieter startup followed by
noisier reception. Startup windows must reject quiet observations outside the
calibration interval, and a valid data bit must survive split illuminated observations. Run it
independently with
`python3 e2e/infrared.py`; capture provenance is in `fixtures/README.md`.
The same command runs a fast C circuit sweep through the production decoder:
capacitance (10–50 pF), pull-down resistance (30–80 kΩ), input trip voltage
(1.2–2.0 V), photocurrent, 120 Hz ambient ripple, a moving shadow, input noise,
clock offset, pulse phase, and peripheral-write delays vary independently.
It compares complete byte streams, including silence before and after them.
A separate weak-signal sweep uses 3.5 µA at 19 pF/45 kΩ/1.55 V; it does not
assume that current produces equal sensitivity at every component extreme.
The C sweep approximates acquisition timing; the full firmware journeys below
execute the actual Xtensa sampler.

## Buffered motion and pocket energy

After `journeys.py` creates `send/flash.bin`, run:

```sh
python3 e2e/motion.py /path/to/esp32s3_rev0_rom.elf build/firmware \
  /path/to/journeys/send/flash.bin build/motion
```

This reuses `pocket-walk` with frequent timer wakes as the chronological reference,
then four-second walking batches at nominal and ±1.5% sensor clocks. Both execute the
same firmware and physical acceleration trace across midnight. It compares total,
daily and archived steps, including a changing-cadence trace. Add
`--reference-firmware /path/to/earlier/build` to compare that trace against an
earlier acquisition design as well as frequent servicing. The journey also checks pickup after rest, a partial
button wake and recovery from an overwritten sensor FIFO. It does not replace
Nintendo's pedometer with an expected steps-per-second formula.

The model records FIFO frames at the configured ODR while the CPU sleeps, dates
them with SENSORTIME, and enforces APS-off FIFO access and write guards. A full
FIFO loses its oldest frames. The overrun check injects an unserviced sensor
backlog; CPU hangs belong to the separate watchdog journey.

`residency.csv` reports sleep entries, active CPU fraction, IMU bytes, total PM1
transactions, and a component-current range for walking and still-after-walking.
The [power budget](../docs/architecture.md#pocket-power-budget) gives the coefficients and supply assumptions. CPU time
already includes I²C and wake overhead. Workload gates use actual wake/rail/step
behavior; predicted days are estimates, not pass criteria. To compare an earlier
firmware using the same model, set `STICK_POWER_BASELINE=1` for `pocket-walk`;
this skips only the new batching and FIFO-specific requirements.

## Connection lifecycle stress

`python3 e2e/connections.py ROM.elf firmware-directory registered.eeprom course.eeprom ../bench --seed 1`

Runs send and return for hot startup, delayed alignment, a damaged burst followed
by retry, and discovery with no peer followed by retry. Delayed alignment
also loses the final shutdown packet, as when the user puts the DS down after
it reports success. The source-derived HGSS
engine advances every millisecond, including when the Stick sends nothing. Its
small adapter lives in `peer.c`; the existing bench supplies Nintendo sources.
The HGSS engine listens for discovery; independent unpaired discovery traffic
also crosses the hot-start sampler window and stops on hearing the first Walker probe.
Hot startup must accept the first reply, using the existing fast 140 µs turnaround; other stories use 380 µs. We do not invent a DS retry loop:
when HGSS terminates a damaged transfer, the next user attempt must succeed in
the same Stick boot.

A seed schedules groups of three 4–30-cycle APB write delays, about 4 µs
apart every 527 µs, alongside 120 Hz ambient variation and 5 mV input noise.
The writes change the pad at their delayed completion time; dedicated GPIO
reads remain CPU-local instructions. These are exploratory disturbances, not
claimed electrical tolerance limits. Packet timing comes from the peer and RMT.
LCD SPI overlapping optical acquisition fails the journey.

Every story runs on the 3.5 µA weak path, the 50 pF/80 kΩ slow path, and the
500 µA bright path. Weak reception must complete transfers under stress;
it is no longer exempted as an intentional failure case.
Each story checks the peer result, bounded completion, working controls, no reboot,
persisted EEPROM, pairing, and course. Return boots the send's actual saved flash,
and the completed return is rebooted and checked again. Successful traces are temporary;
failures retain one directory under `build/connection-failure-*` and print the seed.
Use `--story hot|delayed|glitch|retry` and `--path weak|slow-pad|bright` to run one case while iterating.

Pocket `residency.csv` separates modeled component current from unmeasured board
overhead and reports `memory_off_fraction` from RTC power-control transitions.
The sleep/wake journey presses Front and Side just after sleep entry and later
in sleep, rejects flash off intervals below 2 ms, and requires working input,
rendering and USB recovery. The RTC model honors the programmed minimum sleep
interval; it does not estimate capacitor discharge.
