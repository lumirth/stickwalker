# Debugging Stickwalker

Use the ELF from the firmware you are testing; release ZIPs include it alongside
the application binary. [Back up the flash](install.md#back-up-before-writing)
before installing a diagnostic build so you can restore the current game data.

## Startup or a disappearing USB connection

USB power keeps the ESP32 awake. Use it to inspect startup, PMIC registers and
normal execution. Opening a serial port or changing DTR/RTS can reset the board,
and ESP32 Light-sleep interrupts USB Serial/JTAG. Record resets and port changes
alongside the firmware's reset reason.

After flashing, check the application descriptor, ready state and advancing
RTC. These establish that the application has booted. A flash verification
checks the stored bytes.

## Battery sleep and wake

Arm a finite diagnostic trial over USB, disconnect it, then reconnect after the
trial. Retained records survive the USB reset and contain sample counts, sleep
returns, elapsed time, wake sources and entry errors. Save incomplete records
too; they help locate where execution stopped.

A pending wake source or a sleep interval shorter than entry overhead can
cancel a sleep attempt. Record the API stage and error code to distinguish
these cases from a configuration failure. For power consumption, measure
current at the battery connection using the [power guide](power.md).

## Breakpoints and register inspection

Use Espressif OpenOCD with `board/esp32s3-builtin.cfg` and the matching ELF.
Keep USB connected and place hardware breakpoints before or after infrared
acquisition. Halting inside the optical sampler changes the signal timing.
Watchpoint 1 is reserved for stack-end detection.

Set `ESP_FLASH_SIZE 0` before loading the board configuration to disable flash
breakpoints. If CPU1 inspection fails with an invalid OCD ID, try
`set ESP_ONLYCPU 1` before loading the configuration. This allowed CPU0 reads
in the [recorded board investigation](history/power-and-wake.md).
Restart the board without the debugger when testing watchdogs or panic behavior.

## Crash dumps and PMIC failures

The coredump partition starts at `0x7F0000` and occupies `0x10000` bytes. Decode
a saved dump with its matching ELF and check its integrity and application
identity. Record when you collected it, since flash can retain an older dump.
If no dump was written, inspect the reset reason and power supply, then check
whether execution is stuck or just the USB connection has dropped.

For a failed PMIC read, capture the I2C address, register, attempt number,
duration, error code and reset reason before retrying. Use M5Unified's existing
I2C bus and bounded reinitialization. Measuring SDA and SCL after the failure
can show a stuck bus; a trace during the transfer is needed to locate the fault.

The [hardware references](../../docs/references.md#stick-hardware-and-runtime)
link the vendor's debugging, sleep and peripheral documentation.
