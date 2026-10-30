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

The tool first checks that the firmware supports read-only capture commands. It then saves `session.txt` and `rx.bin`. It does not install firmware or change the saved Walker data. A result with zero retained gates means there is no failed optical burst to replay.

The result includes the build ID, diode preflight state, last accepted command and protocol phase, framing and checksum failures, late samples, long activity, decoding time and reply timing. The raw burst can contain transferred data. Keep it private unless you choose to share it for diagnosis.

## Update a paired Stick

Follow [Update](install.md#update) and make a full flash backup first. A diagnostic capture does not require a reset or reinstall.
