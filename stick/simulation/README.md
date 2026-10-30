# Firmware simulation

`python stick/check.py` builds the production firmware, executes its ESP32-S3
instructions, drives physical GPIO/optical inputs, and compares outgoing packets
and durable EEPROM contents with public fixtures. It does not supply decoded
bytes or synthetic receiver samples to the firmware.

The simulator revision is pinned in `bootstrap.py`. Changes to its peripheral
model are retained in `soc.patch`; the prepared simulator and every verification
run record hashes of that patch, the board/peer sources, the executable, the ROM
support file, and the firmware inputs and outputs.

## Optical inputs

Existing scenarios retain the ideal optical model: a high GPIO charges the node,
and any light while the GPIO is released discharges it. Dark readouts remain
right-censored. This exercises the receiver's treatment of bounded observations.

The optional action below selects a continuous circuit model. Its inputs are
resistance in ohms, capacitance in picofarads, input threshold in volts, signal
photocurrent in microamps, and background photocurrent in microamps:

```
peer circuit 45000 11 1.5 80 0.05
```

The model integrates `C dV/dt = -I_light + I_pull` between actual GPIO drive,
IO_MUX pull, and light-pulse transitions. It emits an input edge when the node
crosses the specified threshold. It knows nothing about acquisition gates,
decoder contrast thresholds, expected bits, or accepted packet checksums.

`peer waveform PHASE PULSE GAP` specifies a phase offset, pulse width, and extra
interbyte gap, all in 240 MHz CPU cycles. Packet data still comes from the peer's
protocol state machine. The new complete-transfer scenarios include:

| Scenario | Circuit / waveform change |
| --- | --- |
| `peer-rc-low` | 22 kΩ, 12 pF, 1.6 V threshold, 60 µA signal, 0.05 µA background |
| `peer-rc-high` | 65 kΩ, with the other low-profile parameters unchanged |
| `peer-rc-gap` | 45 kΩ, 11 pF, 1.5 V, 80 µA signal; 114817 baud, 339-cycle pulses, 4166-cycle extra byte gaps, 347-cycle phase offset |
| `peer-immediate` | Ideal optical model, local transmitter echo, and a peer reply beginning 1 µs after the completed transmission |
| `peer-deadline` | Initial eight-byte FA begins 97.2 ms after FC and finishes after the Walker's 97.7 ms receive deadline; F8 and the complete transfer must meet the peer's unchanged 100 ms response limit |
| `peer-keepalive` | A valid ignored `0x2E` packet precedes each established-link request, with a 100 µs physical frame separation |
| `peer-keepalive-only` | A train of 100 valid ignored packets; actual GPIO charge transitions must stop within 120 ms of the final transmitted reply |

These are illustrative physical inputs, not manufacturer-guaranteed tolerance
ranges. The RC model has constant capacitance, resistance and photocurrent, a
fixed input threshold, and ideal high-level recharge. It does not establish
measured noise, nonlinear diode capacitance, ambient flicker, GPIO latency,
interrupt interference, alignment, or optical recovery margins on hardware.

`peer delay-frame COMMAND DELAY_US` delays the first matching hexadecimal
command relative to the previous Walker transmission, without changing the
peer's response timeout. A 136-byte EEPROM page delayed until 86.2 ms was also
tried: acquisition received the complete page, but its durable journal write
needs at least five 300 µs SPI programs plus a 694 µs acknowledgement. That
cannot fit after the observed 98.234 ms service start with a 100 ms peer limit.
The deadline scenario therefore uses the valid late FA/F8 exchange, while the
ordinary full transfers cover maximum pages and durable storage.

The existing `peer-storage` page ends with the valid eight-byte FA packet
`fa013aa913579bdf`. Readback before and after reboot must preserve those bytes
as page data, exercising admission of a valid outer packet with a valid nested
suffix without using the receiver's implementation as the expected result.

## Transmit DMA

The simulator connects ESP32-S3 GDMA trigger 9 to RMT TX channel 3 when its DMA
access bit is set. It reads descriptors and payloads from emulated SRAM, checks
ownership and length, feeds a 48-symbol FIFO, and advances output symbols by
their programmed duration. Descriptor ownership and DMA EOF interrupts follow
transfer into the FIFO; physical RMT completion follows the output symbols and
their terminator. Malformed descriptors raise DMA/RMT errors.

This exercises the pinned IDF driver, descriptor chain, complete frame duration,
physical completion interrupt, and local optical echo. DMA bus contention and
memory arbitration latency are not modeled. The firmware's complete frame must
already fit in its prepared DMA buffer; hardware timing qualification is still
required.

Optical scenarios retain an SRAM snapshot at each peer report. It can be mapped
with the matching retained ELF to inspect ring observations and counters after a
failure without adding instructions to the firmware's timing-sensitive path.
For timing diagnosis, `STICK_E2E_PIN_TRACE=1` also appends the first 8,192 GPIO5
output-enable and IO_MUX pull events to the peer report. These are timestamps of
the actual emulated peripheral writes; recording them adds no guest instructions
or simulated cycles and does not alter the optical model.
