# Debugging Stickwalker

Start with the firmware's commit, app hash and matching ELF. Preserve the full
flash before writing a diagnostic build; registration and game data live there.
The [installation guide](install.md) describes backup and app-only updates.

## Use USB for awake diagnosis

USB power vetoes Light-sleep. Keep that guard while inspecting startup, PMIC
registers, rail configuration or awake execution. Manual sleep can interrupt the
USB Serial/JTAG transport; a disappearing connection alone does not establish
a firmware crash. Opening a serial connection or changing DTR/RTS can reset the
board, so record those actions as part of the experiment.

After flashing or resetting, verify a valid application descriptor, ready state
and advancing RTC. A successful flash digest or `esptool run` exit alone does
not prove the application is running.

## Test battery sleep with retained records

Arm a finite diagnostic trial over USB, unplug and confirm battery supply,
then reconnect after completion. Preserve completed and incomplete records.
Record sample counts, sleep returns, elapsed time, entry errors, wake sources,
missed deadlines, reset reason and rail configuration. A failed voltage read
means unknown supply state.

Sleep rejection for a pending wake source and an interval too short for entry
are ordinary scheduler outcomes. Record the API stage and error code separately
from setup or driver faults. Sleep counts and voltage readings do not measure
energy; runtime needs whole-device current measurement at the battery path.

## Keep the debugger out of optical acquisition

Use Espressif OpenOCD with `board/esp32s3-builtin.cfg` and the exact ELF while
sleep is vetoed. Prefer hardware breakpoints at startup or after acquisition.
Set `ESP_FLASH_SIZE 0` before loading the target configuration when flash
breakpoints must be disabled. Never halt or instrument the optical gate during
reception: that changes the timing being investigated. Watchpoint index 1 is
reserved for stack-end detection in this build.

A previous dual-core probe returned an invalid CPU1 OCD ID and inconsistent
memory reads. `set ESP_ONLYCPU 1` before loading the board configuration enabled
a valid CPU0 read. Verify descriptor and ELF identity before interpreting values;
that workaround does not explain CPU1's failure. Debugger attachment can alter
watchdog and panic behavior. Repeat those tests after a fresh standalone boot.

## Investigate startup and crashes

Decode a saved core dump against its matching ELF and verify its integrity and
application identity. The selected layout puts coredump at `0x7F0000`, size
`0x10000`. An old valid dump is not a timestamp for the current failure, and an
empty dump does not exclude transport loss, brownout or a non-panicking hang.

For first PMIC-read failures, record reset reason, attempt, address/register,
transfer duration, successful read bytes, error stage and lower-level status
before recovery clears it. High SDA/SCL after a failure only establishes that
the lines are released then. Keep recovery bounded and use M5Unified's owned
bus; a second I2C master or speculative bus pulses can obscure the cause.

See [hardware evidence](history/power-and-wake.md) for the retained failed probes
and [vendor references](../../docs/references.md#stick-hardware-and-runtime) for
the underlying APIs and hardware documentation.
