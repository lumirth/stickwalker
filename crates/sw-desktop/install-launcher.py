#!/usr/bin/env python3
"""Add the extracted Stickwalker application to the current user's app menu."""
import os
from pathlib import Path


def value(text):
    return str(text).replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")


def executable(path):
    # Desktop Entry Exec has a second quoting layer and expands '%' field codes.
    reserved = {'"', "\\", "$", "`"}
    text = "".join("\\" + c if c in reserved else c for c in str(path))
    return value('"' + text.replace("%", "%%") + '"')


def install(root, data):
    entry, = root.glob("*.desktop")
    source = entry.read_text()
    source = source.replace("Exec=stickwalker\n", f"Exec={executable(root / 'stickwalker')}\n")
    source = source.replace(f"Icon={entry.stem}\n", f"Icon={value(root / 'icon.png')}\n")
    applications = data / "applications"
    applications.mkdir(parents=True, exist_ok=True)
    target = applications / entry.name
    target.write_text(source)
    return target


if __name__ == "__main__":
    data = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share")
    print(install(Path(__file__).resolve().parent, data))
