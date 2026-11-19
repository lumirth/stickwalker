#!/usr/bin/env python3
"""Build firmware or desktop packages locally; CI calls these same commands."""

import argparse
import base64
import binascii
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import sys
import tarfile
import tomllib

root = Path(__file__).resolve().parents[1]


APP_ID = (root / "crates/sw-desktop/app-id").read_text().strip()
APPCAST_NAME = "appcast-{arch}.xml"
RELEASE_URL = "https://github.com/lumirth/stickwalker/releases/download"
DESKTOP_ENTRY = f"{APP_ID}.desktop"
FIRMWARE_IMAGES = {
    "bootloader.bin": "bootloader",
    "partition-table.bin": "partitions",
    "stickwalker.bin": "application",
}

def required(name):
    value = os.environ.get(name, "").strip()
    if not value:
        raise SystemExit(f"Set {name} before packaging a release.")
    return value


def update_key():
    key = required("STICKWALKER_UPDATE_KEY")
    try:
        if len(base64.b64decode(key, validate=True)) == 32:
            return key
    except (ValueError, binascii.Error):
        pass
    raise SystemExit("STICKWALKER_UPDATE_KEY must be a base64 Ed25519 public key.")


def signing_identity():
    identity = required("STICKWALKER_SIGN_IDENTITY")
    if not identity.startswith("Developer ID Application: "):
        raise SystemExit("Distribution requires a Developer ID Application identity.")
    return identity


def desktop_version():
    return tomllib.loads((root / "crates/sw-desktop/Cargo.toml").read_text())["package"]["version"]


def check_desktop_release(tag):
    if tag != "desktop-v" + desktop_version():
        raise SystemExit("The desktop release tag does not match the package version.")
    update_key()
    if sys.platform == "darwin":
        signing_identity()


def partitions():
    with (root / "partitions.csv").open() as f:
        rows = csv.reader(line for line in f if line.strip() and not line.lstrip().startswith("#"))
        return {row[0].strip(): dict(offset=int(row[3], 0), size=int(row[4], 0)) for row in rows}


def submit_notarization(path):
    args = ["xcrun", "notarytool", "submit", str(path),
            "--keychain-profile", required("STICKWALKER_NOTARY_PROFILE")]
    if keychain := os.environ.get("STICKWALKER_NOTARY_KEYCHAIN"):
        args.extend(["--keychain", keychain])
    subprocess.run([*args, "--wait"], check=True)


def firmware():
    build = root / "build/firmware"
    out = root / "dist/firmware"
    out.mkdir(parents=True, exist_ok=True)
    images = []
    layout = partitions()
    flash = json.loads((build / "flasher_args.json").read_bytes())
    for kind, key in [("bootloader", "bootloader"), ("partitions", "partition-table"), ("application", "app")]:
        source = flash[key]["file"]
        offset = int(flash[key]["offset"], 0)
        name = Path(source).name
        content = (build / source).read_bytes()
        shutil.copyfile(build / source, out / name)
        images.append(
            dict(
                kind=kind,
                offset=offset,
                file=name,
                sha256=hashlib.sha256(content).hexdigest(),
            )
        )
    (out / "firmware.json").write_text(
        json.dumps(
            dict(
                schema=1,
                product="StickS3",
                version=(root / "VERSION").read_text().strip(),
                storage="SWK6",
                state=layout["walker2"],
                settings=layout["nvs"],
                images=images,
            ),
            indent=2,
        )
        + "\n"
    )
    check_firmware()
    print(out / "firmware.json")


def check_firmware(firmware_dir=None):
    if firmware_dir is None:
        firmware_dir = root / "dist/firmware"
    manifest_file = firmware_dir / "firmware.json"
    if not manifest_file.is_file():
        raise SystemExit(f"Missing firmware manifest: {manifest_file}")
    manifest = json.loads(manifest_file.read_bytes())
    if (manifest.get("schema"), manifest.get("product"), manifest.get("storage")) != (1, "StickS3", "SWK6"):
        raise SystemExit("Invalid Stickwalker firmware manifest.")
    if manifest["version"] != (root / "VERSION").read_text().strip():
        raise SystemExit("Firmware version does not match VERSION.")
    images = manifest["images"]
    if (len(images) != len(FIRMWARE_IMAGES)
            or {image["file"]: image["kind"] for image in images} != FIRMWARE_IMAGES):
        raise SystemExit("The firmware package must include the bootloader, partition table, and application.")
    layout = partitions()
    regions = {"bootloader": (0, 0x8000), "partitions": (0x8000, 0x1000),
               "application": (layout["factory"]["offset"], layout["factory"]["size"])}
    for image in images:
        path = firmware_dir / image["file"]
        if not path.is_file() or path.stat().st_size == 0:
            raise SystemExit(f"Missing or empty firmware image: {path.name}")
        kind = image["kind"]
        offset, capacity = regions[kind]
        if image["offset"] != offset or path.stat().st_size > capacity:
            raise SystemExit(f"{path.name} does not fit its flash region.")
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != image["sha256"].lower():
            raise SystemExit(f"Checksum mismatch for firmware image: {path.name}")
        if kind == "application":
            if len(data) < 208 or data[0] != 0xe9 or struct.unpack_from("<I", data, 32)[0] != 0xabcd5432:
                raise SystemExit("Invalid ESP application descriptor in stickwalker.bin.")
            if data[48:80].rstrip(b"\0") != manifest["version"].encode("ascii"):
                raise SystemExit("The version in stickwalker.bin does not match firmware.json.")

    table = (firmware_dir / "partition-table.bin").read_bytes()
    actual = {}
    for at in range(0, len(table) - 31, 32):
        magic, _, _, offset, size, label, _ = struct.unpack_from("<HBBII16sI", table, at)
        if magic != 0x50AA:
            break
        actual[label.rstrip(b"\0").decode("ascii")] = dict(offset=offset, size=size)
    if (actual != layout or manifest["state"] != layout["walker2"]
            or manifest["settings"] != layout["nvs"]):
        raise SystemExit("The firmware package and partition table disagree.")


def desktop(a):
    target = Path(os.environ.get("CARGO_TARGET_DIR", root / "target"))
    if sys.platform == "darwin":
        import dmgbuild

    mode = "debug" if a.debug else "release"
    if not a.no_build:
        subprocess.run(
            [
                "cargo",
                "build",
                "--locked",
                "-p",
                "sw-desktop",
                *([] if a.debug else ["--release"]),
            ],
            cwd=root,
            check=True,
        )
    version = desktop_version()
    out = root / "dist/desktop"
    out.mkdir(parents=True, exist_ok=True)
    if sys.platform == "darwin":
        app = out / "Stickwalker.app"
        if app.exists():
            shutil.rmtree(app)
        (app / "Contents/MacOS").mkdir(parents=True)
        shutil.copy2(
            target / mode / "stickwalker", app / "Contents/MacOS/stickwalker"
        )
        (app / "Contents/Resources").mkdir()
        shutil.copy2(root / "crates/sw-desktop/assets/icon.icns", app / "Contents/Resources/icon.icns")
        info = dict(
            CFBundleIconFile="icon.icns",
            CFBundleName="Stickwalker",
            CFBundleDisplayName="Stickwalker",
            CFBundleIdentifier=APP_ID,
            CFBundleExecutable="stickwalker",
            CFBundlePackageType="APPL",
            CFBundleVersion=version,
            CFBundleShortVersionString=version,
            NSHighResolutionCapable=True,
            LSMinimumSystemVersion="12.0",
        )
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps(info))
        if a.sign:
            identity = signing_identity()
            key = update_key()
            arch = os.uname().machine
            feed = os.environ.get(
                "STICKWALKER_APPCAST",
                f"{RELEASE_URL}/desktop-feed/{APPCAST_NAME.format(arch=arch)}",
            )
            subprocess.run(
                [
                    "gpui-updater-release",
                    "sparkle",
                    "embed",
                    "--app",
                    str(app),
                    "--feed-url",
                    feed,
                    "--public-key",
                    key,
                    "--automatic-checks",
                    "false",
                ],
                check=True,
            )
            embedded = plistlib.loads((app / "Contents/Info.plist").read_bytes())
            if embedded.get("SUFeedURL") != feed or embedded.get("SUPublicEDKey") != key:
                raise SystemExit("Sparkle embedded a different update feed or signing key.")
            subprocess.run(
                [
                    "gpui-updater-release",
                    "sparkle",
                    "codesign",
                    "--app",
                    str(app),
                    "--identity",
                    identity,
                ],
                check=True,
            )
            subprocess.run(
                [
                    "codesign",
                    "--force",
                    "--options",
                    "runtime",
                    "--timestamp",
                    "--sign",
                    identity,
                    str(app),
                ],
                check=True,
            )
            subprocess.run(
                ["codesign", "--verify", "--deep", "--strict", str(app)], check=True
            )
            archive = out / "notarize.zip"
            subprocess.run(
                ["ditto", "-c", "-k", "--keepParent", str(app), str(archive)],
                check=True,
            )
            submit_notarization(archive)
            subprocess.run(["xcrun", "stapler", "staple", str(app)], check=True)
            subprocess.run(
                ["spctl", "--assess", "--type", "execute", "--verbose", str(app)],
                check=True,
            )
            archive.unlink()
        else:
            subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True)
        dmg = out / f"Stickwalker-{version}-macos-{os.uname().machine}.dmg"
        assets = root / "crates/sw-desktop/assets"
        dmgbuild.build_dmg(
            str(dmg), "Stickwalker",
            settings={
                "format": "UDZO",
                "files": [str(app)],
                "symlinks": {"Applications": "/Applications"},
                "icon": str(assets / "icon.icns"),
                "background": str(assets / "dmg/background.png"),
                "window_rect": ((200, 200), (640, 420)),
                "icon_locations": {app.name: (166, 220), "Applications": (474, 220)},
                "icon_size": 112,
                "text_size": 14,
            },
        )
        if a.sign:
            subprocess.run(
                ["codesign", "--timestamp", "--sign", identity, str(dmg)], check=True
            )
            submit_notarization(dmg)
            subprocess.run(["xcrun", "stapler", "staple", str(dmg)], check=True)
        print(dmg)
        print(app)
    elif sys.platform == "win32":
        dest = out / f"Stickwalker-{version}-windows-x86_64.exe"
        shutil.copy2(target / mode / "stickwalker.exe", dest)
        print(dest)
    else:
        archive = out / f"Stickwalker-{version}-linux-{os.uname().machine}.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(
                target / mode / "stickwalker",
                arcname="Stickwalker/stickwalker",
            )
            tar.add(root / "crates/sw-desktop/assets/icon.png", arcname="Stickwalker/icon.png")
            template = (root / "crates/sw-desktop/stickwalker.desktop").read_text()
            entry = template.replace("@APP_ID@", APP_ID).encode()
            info = tarfile.TarInfo(f"Stickwalker/{DESKTOP_ENTRY}")
            info.size = len(entry)
            tar.addfile(info, io.BytesIO(entry))
            tar.add(root / "crates/sw-desktop/install-launcher.py", arcname="Stickwalker/install-launcher.py")
            tar.add(
                root / "crates/sw-desktop/60-stickwalker.rules",
                arcname="Stickwalker/60-stickwalker.rules",
            )
        print(archive)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("firmware", help="package the existing ESP-IDF build")
    commands.add_parser("check-firmware", help="verify the firmware version and image checksums")
    release = commands.add_parser("check-desktop-release", help="verify inputs for a desktop release")
    release.add_argument("tag")
    app = commands.add_parser("desktop", help="build and package the native app")
    app.add_argument("--debug", action="store_true")
    app.add_argument("--no-build", action="store_true")
    app.add_argument(
        "--sign", action="store_true", help="require Developer ID and notarization"
    )
    args = parser.parse_args()
    if args.command == "firmware":
        firmware()
    elif args.command == "check-desktop-release":
        check_desktop_release(args.tag)
    elif args.command == "check-firmware":
        check_firmware()
    else:
        desktop(args)
