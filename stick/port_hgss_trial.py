#!/usr/bin/env python3
"""One source-faithful HGSS trial with Stick-owned protocol decisions."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import sys
import time
from pathlib import Path

import serial

BENCH_HOST = Path(__file__).resolve().parents[2] / "bench" / "host"
sys.path.insert(0, str(BENCH_HOST))
from g5_hgss_entry import make_source  # noqa: E402
from hgss_trials import read_trace, start_peer  # noqa: E402
from irctl import Client  # noqa: E402


def save(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n")


def lua_payload(name: str, payload: bytes) -> str:
    encoded = base64.b64encode(payload).decode("ascii")
    chunks = "..\n".join(repr(encoded[i:i + 800]) for i in range(0, len(encoded), 800))
    return (
        "local alphabet='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'\n"
        "local function from64(s)\n"
        " local out={} local acc=0 local bits=0\n"
        " for i=1,#s do local c=s:sub(i,i) if c~='=' then\n"
        "  local v=assert(alphabet:find(c,1,true))-1\n"
        "  acc=acc*64+v bits=bits+6\n"
        "  if bits>=8 then bits=bits-8 local q=math.floor(acc/2^bits)\n"
        "   out[#out+1]=string.char(q%256) acc=acc%2^bits end\n"
        " end end return table.concat(out) end\n"
        f"local {name}=from64({chunks})\n"
    )


def source_with_image(image: bytes) -> str:
    if len(image) != 35920:
        raise ValueError("HGSS entry image must be exactly 35920 bytes")
    setup = lua_payload("image", image)
    source = make_source(duration_ms=16000, random_payloads=True)
    source = source.replace("local image=lab.random(35920)\n", setup, 1)
    if len(source.encode()) > 65536:
        raise ValueError("3DS script source exceeds its 64 KiB limit")
    return source


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--port", default="/dev/cu.usbmodem101")
    parser.add_argument("--image", type=Path,
                        help="HGSS source-built resource image; other sealed inputs remain fresh")
    parser.add_argument("--course", type=Path,
                        help="10,430-byte source-backed course fixture for put")
    parser.add_argument("--mode", choices=("entry", "put", "back", "present", "cleanup", "cleanup_all"),
                        default="entry", help="Original HGSS PHC operation")
    args = parser.parse_args()
    run = args.run.resolve()
    run.mkdir(parents=True, exist_ok=False)
    if args.mode == "entry":
        if args.course:
            raise ValueError("--course is only an input to put")
        source = (source_with_image(args.image.read_bytes()) if args.image else
                  make_source(duration_ms=16000, random_payloads=True))
    else:
        if args.image:
            raise ValueError("--image is only an input to the entry operation")
        if args.mode == "put" and not args.course:
            raise ValueError("put requires an explicit, reviewed --course fixture")
        if args.course and args.mode != "put":
            raise ValueError("--course is only an input to put")
        course_setup = ""
        course_field = ""
        if args.course:
            course = args.course.read_bytes()
            if len(course) != 10430:
                raise ValueError("HGSS put course must be exactly 10430 bytes")
            course_setup = lua_payload("course", course)
            course_field = ",course=course"
            save(run / "course-input.json", dict(path=str(args.course.resolve()),
                 length=len(course), sha256=hashlib.sha256(course).hexdigest()))
        source = course_setup + (
            'ir.configure{i2c="mapped"}\n'
            f'local result=hgss.run{{mode="{args.mode}",duration_ms=16000,'
            f'sealed=true,compress=true{course_field}}}\n'
            'lab.mark(string.format("received=%d sent=%d rejected=%d '
            'overruns=%d connected=%s",result.received,result.sent,'
            'result.rejected,result.overruns,tostring(result.connected)))\n'
        )
    (run / "source.lua").write_text(source)
    with Client.from_config() as peer, serial.Serial(args.port, 115200, timeout=.1) as stick:
        state = peer.status()
        save(run / "before.json", state)
        if state["state"] == "running" or not state["armed"]:
            raise RuntimeError("3DS is not armed and idle")
        lines: list[str] = []
        stick.write(b"t")
        start = time.monotonic()
        while time.monotonic() - start < 3:
            line = stick.readline().decode(errors="replace").strip()
            if line:
                lines.append(line)
                if line.startswith("PW_STICK_UI"):
                    break
        else:
            raise RuntimeError("Stick did not answer the liveness query")
        job = start_peer(peer, source, 20000)
        stick.write(b"c")
        started = time.monotonic()
        while time.monotonic() - started < 30:
            line = stick.readline().decode(errors="replace").strip()
            if line:
                lines.append(line)
                if line.startswith("PW_STICK_IR_DONE"):
                    break
        (run / "stick-log.txt").write_text("\n".join(lines) + "\n")
        predictions = [dict(length=int(m.group(1)), wire=m.group(2))
                       for line in lines
                       if (m := re.fullmatch(r"PW_STICK_TRIAL_FRAME length=(\d+) tick=\d+ wire=([0-9a-f]+)", line))]
        burst_rows = [line for line in lines if line.startswith("PW_STICK_BURST")]
        prediction = dict(job=job, stick_done=next((line for line in lines
            if line.startswith("PW_STICK_IR_DONE")), None),
            receiver_stats=next((line for line in lines
            if line.startswith("PW_STICK_IR_STATS")), None), bursts=burst_rows,
            frames=predictions)
        save(run / "prediction.json", prediction)
        final = peer.wait(job, maximum_seconds=22)
        save(run / "status.json", final)
        commit = peer.commit((run / "prediction.json").read_bytes())
        (run / "commitment.txt").write_text(commit + "\n")
        peer.reveal()
        peer.export(run / "3ds")
        trace = read_trace(run / "3ds" / "hgss-trace.bin")
        save(run / "trace.json", trace)
        states = [row["state"] for row in trace["events"] if row["kind"] == 3]
        print(json.dumps(dict(job=job, commitment=commit, prediction_sha256=
            hashlib.sha256((run / "prediction.json").read_bytes()).hexdigest(),
            stick_done=prediction["stick_done"], receiver_stats=prediction["receiver_stats"],
            bursts=burst_rows, last_peer_state=states[-1] if states else None), indent=2))


if __name__ == "__main__":
    main()
