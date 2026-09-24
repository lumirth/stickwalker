#!/usr/bin/env python3
"""Run whole original-PHC back/put cycles and preserve every attempt.

The host starts one complete source peer operation at a time. Packet parsing,
answers, timeouts, and EEPROM decisions stay on the Stick and original 3DS
engine. Stop on the first failure so its optical evidence can be examined.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


EXPECTED = {"back": 90, "put": 133}


def run_one(root: Path, name: str, mode: str, course: Path) -> dict:
    path = root / name
    command = [sys.executable, str(Path(__file__).with_name("port_hgss_trial.py")),
               str(path), "--mode", mode]
    if mode == "put":
        command += ["--course", str(course)]
    result = subprocess.run(command, text=True, capture_output=True)
    (root / f"{name}-stdout.json").write_text(result.stdout)
    (root / f"{name}-stderr.txt").write_text(result.stderr)
    row = {"name": name, "mode": mode, "exit_code": result.returncode,
           "artifact": str(path), "ok": False}
    try:
        report = json.loads(result.stdout)
        stats = report["receiver_stats"] or ""
        valid = int(re.search(r"(?<!in)valid=(\d+)", stats).group(1))
        invalid = int(re.search(r"invalid=(\d+)", stats).group(1))
        peer_error = report["last_peer_state"]["error"]
        stick_done = report["stick_done"]
        row.update(valid=valid, invalid=invalid, peer_error=peer_error,
                   stick_done=stick_done)
        row["ok"] = (result.returncode == 0 and valid == EXPECTED[mode] and
                     invalid == 0 and peer_error == 15 and
                     stick_done == "PW_STICK_IR_DONE result=0")
    except (KeyError, TypeError, ValueError, AttributeError):
        row["failure"] = "incomplete trial report; inspect stderr and artifacts"
    return row


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="new directory for the run")
    parser.add_argument("--course", required=True, type=Path)
    parser.add_argument("--cycles", type=int, default=30)
    parser.add_argument("--recover-put", action="store_true",
                        help="after a failed put, try one new whole put session"
                             " to leave the walker usable; keep the failure")
    args = parser.parse_args()
    if args.cycles <= 0:
        parser.error("cycles must be positive")
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=False)
    course = args.course.resolve()
    if len(course.read_bytes()) != 10430:
        parser.error("course must contain 10,430 bytes")
    ledger: list[dict] = []

    def append(row: dict) -> None:
        ledger.append(row)
        (root / "ledger.json").write_text(json.dumps(ledger, indent=2) + "\n")
        print(json.dumps(row), flush=True)

    for cycle in range(1, args.cycles + 1):
        for mode in ("back", "put"):
            name = f"cycle-{cycle:03d}-{mode}"
            row = run_one(root, name, mode, course)
            append(row)
            if row["ok"]:
                continue
            if mode == "put" and args.recover_put:
                recovery = run_one(root, f"cycle-{cycle:03d}-put-recovery",
                                   "put", course)
                recovery["purpose"] = "new whole session after recorded failure"
                append(recovery)
            raise SystemExit(1)


if __name__ == "__main__":
    main()
