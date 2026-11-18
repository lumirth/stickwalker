"""Use the host diagnostic reader for simulator flash results."""
from functools import cache
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

@cache
def reader():
    subprocess.run(["cargo", "build", "--quiet", "--locked", "-p",
                    "sw-host", "--example", "device"], cwd=ROOT, check=True)
    return ROOT / "target/debug/examples/device"

def decode(data):
    result = subprocess.run([reader(), "decode", "-"], input=data, capture_output=True)
    if result.returncode:
        raise ValueError(result.stderr.decode().strip())
    return result.stdout[:65536], result.stdout[65536:]
