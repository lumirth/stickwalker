#!/usr/bin/env python3
"""Inspect the compiled acquisition path and retain its exact instruction hashes.

This verifies IRAM residency, not electrical timing or hardware qualification.
The old instruction-identity baseline described a different receiver and is
not evidence for the replacement's physical margins.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import tempfile
from pathlib import Path

FUNCTIONS = ("sample_receive_turn",)


def check(elf: Path, prefix: str) -> dict:
    symbols = subprocess.check_output([prefix + "nm", "-C", "-S", str(elf)], text=True)
    sections = subprocess.check_output([prefix + "objdump", "-h", str(elf)], text=True)
    assembly = subprocess.check_output([prefix + "objdump", "-d", "-C", str(elf)], text=True)
    section = re.search(r"\s\.iram0.text\s+([0-9a-f]+)\s+([0-9a-f]+)", sections)
    if not section:
        raise ValueError("Firmware has no IRAM text section")
    size, start = (int(value, 16) for value in section.groups())
    with tempfile.TemporaryDirectory(prefix="stickwalker-timing-") as temp:
        binary = Path(temp) / "iram.bin"
        subprocess.run(
            [
                prefix + "objcopy",
                "-O",
                "binary",
                "--only-section=.iram0.text",
                str(elf),
                str(binary),
            ],
            check=True,
        )
        data = binary.read_bytes()
    results = {}
    for name in FUNCTIONS:
        matches = [
            row
            for row in symbols.splitlines()
            if name + "(" in row and (" t " in row or " T " in row)
        ]
        if len(matches) != 1:
            raise ValueError(f"Expected exactly one compiled {name}, found {len(matches)}")
        address, length = (int(value, 16) for value in matches[0].split()[:2])
        resident = start <= address and address + length <= start + size
        instructions = []
        calls = []
        for line in assembly.splitlines():
            instruction = re.match(r"\s*([0-9a-f]+):\s", line)
            if not instruction or not address <= int(instruction[1], 16) < address + length:
                continue
            instructions.append(line)
            target = re.search(r"\bcall(?:0|4|8|12)\s+([0-9a-f]+)", line)
            if target:
                calls.append(int(target[1], 16))
        indirect = any(re.search(r"\bcallx(?:0|4|8|12)\b", line) for line in instructions)
        safe_calls = all(
            start <= target < start + size or 0x40000000 <= target < 0x40080000 for target in calls
        )
        body = data[address - start : address - start + length] if resident else b""
        results[name] = {
            "address": hex(address),
            "bytes": length,
            "sha256": hashlib.sha256(body).hexdigest(),
            "iram_resident": resident,
            "direct_calls": [hex(target) for target in calls],
            "calls_in_iram_or_rom": safe_calls,
            "indirect_calls": indirect,
        }
    return {
        "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(),
        "verification": "acquisition-iram-residency",
        "hardware_qualified": False,
        "functions": results,
        "passed": all(
            row["iram_resident"] and row["calls_in_iram_or_rom"] and not row["indirect_calls"]
            for row in results.values()
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument(
        "--tool-prefix", required=True, help="path prefix ending in xtensa-esp32s3-elf-"
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = check(args.elf, args.tool_prefix)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
