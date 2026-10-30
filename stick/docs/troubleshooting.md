# Troubleshoot a connection

Check that the BPV10NF anode connects to GND and its cathode connects to G5. Its leads need contact with the socket. Keep tape and marker off the contact areas. Aim the diode toward the cartridge and hold both devices still during the exchange.

Use the [margin indicator](controls.md) to distinguish a detected diode from received traffic. A detected diode does not guarantee enough optical signal for a transfer.

If a connection is intermittent, try a shaded position away from direct sunlight and lamps. Change one condition at a time. Darkness alone does not rule out contact, aiming or decoding problems.

Open **Last connection** in Stick Settings for the last connection result and its observations. For a failed exchange, collect the diagnostic result before starting another attempt.

## Collect a diagnostic result

Keep the Stick awake and connect it with a USB data cable. From the source checkout, replace `PORT` and choose a new output directory:

```sh
uv run --with pyserial==3.5 python stick/capture_link.py --port PORT --output stick/.build/link-capture
```

The tool first checks that the firmware supports read-only capture commands. It then saves `session.txt` and `rx.bin`. It does not install firmware or change the saved Walker data. A result with zero retained gates means no receive window was retained.

The result includes the build ID, diode preflight state, last accepted command and protocol phase, framing and checksum failures, acquisition gaps, late samples, transport faults, reply timing, and transmit-bound uncertainty. Version 2 also records the raw window's gate indices, time origin, and local-transmit exclusion boundary. It retains up to 32,768 gates from one continuous receive interval; its beginning can be partway through a packet. The raw window can contain transferred data. Keep it private unless you choose to share it for diagnosis.

The receiver has no calibration settings. Background, contact, aiming, saturation, and weak pulses still affect the information available to it. A successful model replay does not qualify a particular physical setup.

## Update a paired Stick

Follow [Update](install.md#update) and make a full flash backup first. A diagnostic capture does not require a reset or reinstall.
