#!/usr/bin/env python3
"""Compare the timing-sensitive IR instruction bytes with the frozen production image."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check(elf: Path, prefix: str) -> dict:
    baseline = json.loads((ROOT / "stick/ir-timing-baseline.json").read_text())
    symbols = subprocess.check_output([prefix + "nm", "-C", "-S", str(elf)], text=True)
    sections = subprocess.check_output([prefix + "objdump", "-h", str(elf)], text=True)
    start = int(re.search(r"\s\.iram0.text\s+[0-9a-f]+\s+([0-9a-f]+)", sections)[1], 16)
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
    for name, expected in baseline["functions"].items():
        row = next(
            r for r in symbols.splitlines() if name in r and (" t " in r or " T " in r)
        )
        address, size = (int(value, 16) for value in row.split()[:2])
        offset = address - start
        body = data[offset : offset + size]
        sha = hashlib.sha256(body).hexdigest()
        results[name] = {
            "bytes": size,
            "sha256": sha,
            "baseline_identical": len(body) == size
            and size == expected["bytes"]
            and sha == expected["sha256"],
        }
    return {
        "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(),
        "reference_app_sha256": baseline["reference_app_sha256"],
        "functions": results,
        "passed": all(r["baseline_identical"] for r in results.values()),
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
