#!/usr/bin/env python3
"""Collect the last connection result and raw receive window without changing saves."""

import argparse
import re
import time
from pathlib import Path


def collect(port):
    import serial

    connection = serial.Serial(port=None, baudrate=115200, timeout=0.2)
    connection.dtr = connection.rts = False
    connection.port = port
    with connection:
        connection.reset_input_buffer()
        connection.write(b"?")
        lines = []
        deadline = time.monotonic() + 5
        acknowledged = False
        while time.monotonic() < deadline:
            line = connection.readline().decode("ascii", errors="replace")
            lines.append(line)
            acknowledged |= bool(re.match(r"PW_STICK_DIAGNOSTICS version=[12] ", line))
            if acknowledged and line.startswith("PW_STICK_LINK "):
                break
        if not acknowledged:
            raise RuntimeError(
                "Firmware did not acknowledge read-only diagnostics; no capture requested"
            )
        # Older diagnostic builds assign other meanings to 'r'. Request it only
        # after the explicit versioned acknowledgement from production firmware.
        connection.write(b"r")
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            line = connection.readline().decode("ascii", errors="replace")
            lines.append(line)
            if line.startswith("PW_STICK_RX_CAPTURE_END"):
                return "".join(lines)
        raise RuntimeError("Capture incomplete; keep the device awake and retry")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port")
    source.add_argument("--log", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="new output directory")
    args = parser.parse_args()
    text = collect(args.port) if args.port else args.log.read_text()
    match = re.search(
        r"PW_STICK_RX_CAPTURE gates=(\d+)[^\r\n]*\r?\n(.*?)PW_STICK_RX_CAPTURE_END", text, re.S
    )
    if not match:
        raise RuntimeError("No complete optical capture in the response")
    count = int(match[1])
    if count > 32768:
        raise RuntimeError("Capture exceeds the firmware limit")
    bins = bytes.fromhex(match[2])
    if len(bins) != count:
        raise RuntimeError(f"Incomplete capture: expected {count} gates, received {len(bins)}")
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "session.txt").write_text(text)
    (args.output / "rx.bin").write_bytes(bins)
    for line in text.splitlines():
        if line.startswith(("PW_STICK_DIAGNOSTICS ", "PW_STICK_LINK ")):
            print(line)
    print(f"Retained {count} optical gates in {args.output.resolve()}")


if __name__ == "__main__":
    main()
