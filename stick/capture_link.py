#!/usr/bin/env python3
"""Collect the last connection result and raw optical observations without changing saves."""

import argparse
import json
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
            acknowledged |= line.startswith("PW_STICK_DIAGNOSTICS version=1 ")
            if acknowledged and line.startswith("PW_STICK_LINK "):
                break
        if not acknowledged:
            raise RuntimeError(
                "Firmware did not acknowledge read-only diagnostics; no capture requested"
            )
        # Older diagnostic builds assign other meanings to 'r'. Request it only
        # after the explicit versioned acknowledgement from production firmware.
        connection.write(b"r")
        # A full 32K-gate u16 hexadecimal dump takes about 12 seconds on a
        # physical 115200-baud UART; USB CDC may finish considerably sooner.
        deadline = time.monotonic() + 20
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
        r"PW_STICK_RX_CAPTURE gates=(\d+) format=u16 first_gate=(\d+) "
        r"end_gate=(\d+) epoch_first=(\d+) epoch_us=(\d+)\r?\n"
        r"(.*?)PW_STICK_RX_CAPTURE_END",
        text,
        re.S,
    )
    if not match:
        raise RuntimeError("No complete u16 optical capture in the response")
    count = int(match[1])
    if count > 32768:
        raise RuntimeError("Capture exceeds the firmware limit")
    first, end, epoch_first, epoch_us = (int(match[i]) for i in range(2, 6))
    if end - first != count or epoch_first > end:
        raise RuntimeError("Capture gate indices are inconsistent")
    encoded = re.sub(r"\s+", "", match[6])
    if len(encoded) != count * 4 or re.search(r"[^0-9a-fA-F]", encoded):
        raise RuntimeError(f"Incomplete capture: expected {count} four-digit observations")
    observations = [int(encoded[i : i + 4], 16) for i in range(0, len(encoded), 4)]
    bins = b"".join(value.to_bytes(2, "little") for value in observations)
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "session.txt").write_text(text)
    (args.output / "rx.bin").write_bytes(bins)
    (args.output / "rx.json").write_text(
        json.dumps(
            {
                "version": 2,
                "encoding": "u16le",
                "gates": count,
                "first_gate": first,
                "end_gate": end,
                "latest_epoch_first_gate": epoch_first,
                "latest_epoch_origin_us": epoch_us,
                "gate_hz": 345600,
                "cpu_hz": 240000000,
                "cycles_mask": 0x3FFF,
                "censored_flag": 0x4000,
                "invalid_flag": 0x8000,
                "finite": "Threshold crossing delay upper bound in CPU cycles",
                "censored": "Input still high at this delay; a lower bound, not a crossing",
                "invalid": "Discontinuity: a missed scheduled measurement or a service boundary",
                "timing": "The time origin describes the latest receive epoch only; earlier service gaps are marked invalid",
            },
            indent=2,
        )
        + "\n"
    )
    for line in text.splitlines():
        if line.startswith(("PW_STICK_DIAGNOSTICS ", "PW_STICK_LINK ")):
            print(line)
    print(f"Retained {count} optical gates in {args.output.resolve()}")


if __name__ == "__main__":
    main()
