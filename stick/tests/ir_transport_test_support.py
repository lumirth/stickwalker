"""Extract production transport functions for standalone driver tests."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "stick/ir_transport.cpp").read_text()


def transport_function(signature):
    start = SOURCE.index(signature)
    opening = SOURCE.index("{", start)
    depth = 1
    cursor = opening + 1
    while depth:
        depth += (SOURCE[cursor] == "{") - (SOURCE[cursor] == "}")
        cursor += 1
    return SOURCE[start:cursor]
