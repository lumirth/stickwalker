# GPIO5 quiet capture

`gpio5-quiet.bin` contains 8,192 chronological observations captured from the
physical StickS3 on 2026-11-14 at the failure boundary of an HGSS connection.
It covers 14.079 ms at the end of the failed receive turn. Discharge times span
337–363 CPU cycles, with no decoded valid peer packet in this window.
Each record is little-endian `(u32 end_cycle, u16 duration, u8 flags,
u8 sample_gap_cycles)`, at 240 MHz. No save, identity, or game bytes are included.

The installed firmware measured only a 17-cycle quiet allowance at startup.
Replaying this window with that allowance produces 25 false bytes and 51 errors;
measuring the full window gives a 30-cycle allowance and zero bytes/errors.
The capture ring was rotated at its timestamp discontinuity, including five
completed samples beyond the last published batch cursor.

`python3 e2e/infrared.py` compiles the production decoder in a temporary
folder and replays the capture using the configured startup sample count. It
tries every 64th calibration start phase and requires no bytes or errors over
the entire quiet capture, including observations outside each calibration
window. The journey runner runs this once before booting the simulator.
This is a focused physical-trace regression, not a whole-device simulation.

## Split pulse during an HGSS data packet

`gpio5-split-pulse.bin` is a 119-observation crop from a later physical send
failure on 2026-11-14. It covers two bytes, `67 59`, with the receiver already
calibrated at a 23-cycle noise allowance and a roughly 356-cycle quiet baseline.
The recorder copied samples into RAM on the first framing error and sent its
report over USB only after the connection ended; no CPU breakpoint was used.

In the first byte, the zero in data-bit slot 5 appears as two illuminated
fragments. Both fit the existing start-time interval, but the old decoder rejects
the second solely because that slot is already zero. Its framing error then
loses both bytes. The corrected decoder intersects the timing bounds for both
fragments and emits `67 59`. Stop-bit and incompatible-timing rejection remain.
The crop contains only these waveform samples, not a save or trainer identity.

## Measured floating phase and polling intervals

`gpio5-timed-quiet.bin` and `gpio5-timed-bus-load.bin` each hold 8,192
observations from the physical StickS3 on 2026-11-14. No peer was transmitting.
The second capture includes CPU0 repeatedly reading and writing a volatile SRAM
array while CPU1 samples GPIO5. A temporary application recorded the ring in
RAM, stopped acquisition, and only then sent it over USB. No debugger was attached.

These records are little-endian `(u32 end_cycle, u16 duration, u8 flags,
u8 sample_gap_cycles, u16 signal_duration, u16 floating_cycles)`. `duration`
and `end_cycle` retain actual pulse timing. `signal_duration` subtracts the
measured floating phase and adds its nominal loop duration, so execution-time
variation does not become a change in the light estimate. The polling interval
includes elapsed cycles beyond the loop's minimum instruction count.

The production decoder emits no bytes or errors in either quiet capture, both
with its default noise allowance and across 65 startup calibration windows.
With the original four-cycle allowance, two negative controls on the bus-load
capture establish why both measurements matter: substituting raw duration for
the light estimate produces 10 false bytes and 7 errors; replacing the measured polling intervals with a fixed
2-cycle interval produces 7 false bytes and 12 errors. Neither control is used
in the passing fixtures. The tests also retain the earlier real HGSS split-pulse
capture, so rejecting everything cannot pass.

These traces verify the measured receiver/decoder boundary. Complete firmware
send/return journeys separately exercise the optical model, protocol, deadlines,
and persisted EEPROM. Neither kind of test substitutes for the other.

## Quiet line after a failed transfer

`gpio5-post-transfer-quiet.bin` contains the final 8,192 observations (22.190 ms)
retained after a physical HGSS attempt failed on 2026-11-15. It uses the same
12-byte format as the timed captures. The receiver had a four-cycle noise
allowance; replaying these quiet samples with that allowance emits 33 false
bytes and four framing errors. Measuring noise from the same capture conceals
the failure, because it selects a larger allowance.

The regression therefore also calibrates using the earlier, quieter startup
capture and then decodes this one. Both initial and measured allowances must
retain the timing margin when startup is quiet. The captured samples contain
no save or identity data; the existing split-pulse and full-transfer tests
continue to check reception.

## Desktop migration fixtures

`public-raw.bin.zlib` and `public-littlefs.bin.zlib` contain checkpoints and
journal edits emitted by public Stickwalker storage writers at `3e6634d` and
`0e767b3`. The LittleFS file payloads were placed into an actual LittleFS image.
Each has its own independently emitted expected EEPROM. `current.bin.zlib` and
its expected EEPROM come from the real SWK6 writer in `e2e/storage.c`.
These are synthetic, compressed fixtures; no ROM, personal save, network access,
or legacy Arduino compilation is needed to run the migration checks.

`empty-littlefs.bin.zlib` is an empty filesystem made with littlefs-python,
using the public partition's 4096-byte blocks and 384-block geometry. It checks
that a factory/foreign filesystem is installable without being called a damaged walk.

`public-nvs.bin.zlib` contains the 20 KiB Arduino NVS partition from the physical 2026-11-14 preflight flash capture. It includes superseded control entries and the final display preference.

`public-littlefs-pair.bin.zlib` is a synthetic LittleFS 2.1 volume made with littlefs-python 0.19. Its two PWE1 checkpoints have sequences 7 and 8 and carry their sequence at EEPROM offset 0x100. Migration tests damage each payload in turn.
