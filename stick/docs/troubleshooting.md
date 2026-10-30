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

The tool first checks that the firmware supports read-only capture commands. It then saves `session.txt`, `rx.bin` and `rx.json`. The binary contains little-endian 16-bit observations; the JSON records their schema and retained gate range. It does not install firmware or change the saved Walker data. A result with zero retained gates means no acquisition is available. The retained ring can contain more than one receive turn; explicit invalid observations mark turn boundaries. Its 32,768 observations cover about 94.8 ms of sampled time, so early input can age out before a long receive turn ends. The time origin describes the latest receive turn only.

The result includes the build ID, diode preflight state, last accepted command and protocol phase, framing and erasure counts, checksum and packet-admission failures, late samples, prefix resynchronization and reply timing. Reply timing records accepted request-to-reply time, intervals between completed replies and deadline misses. The retained optical observations can contain transferred data. Keep them private unless you choose to share them for diagnosis.

## Update a paired Stick

Follow [Update](install.md#update) and make a full flash backup first. A diagnostic capture does not require a reset or reinstall.
