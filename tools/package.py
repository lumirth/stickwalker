#!/usr/bin/env python3
"""Build firmware or desktop packages locally; CI calls these same commands."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tarfile
import tomllib

root = Path(__file__).resolve().parents[1]


def firmware():
    build = root / "build/firmware"
    out = root / "dist/firmware"
    out.mkdir(parents=True, exist_ok=True)
    images = []
    for offset, source, name in [
        (0, "bootloader/bootloader.bin", "bootloader.bin"),
        (0x8000, "partition_table/partition-table.bin", "partition-table.bin"),
        (0x10000, "stickwalker.bin", "stickwalker.bin"),
    ]:
        content = (build / source).read_bytes()
        shutil.copyfile(build / source, out / name)
        images.append(
            dict(
                kind={0: "bootloader", 0x8000: "partitions", 0x10000: "application"}[
                    offset
                ],
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
                state=dict(offset=0x610000, size=0x1F0000),
                settings=dict(offset=0x9000, size=0x6000),
                images=images,
            ),
            indent=2,
        )
        + "\n"
    )
    print(out / "firmware.json")


def check_firmware():
    firmware_dir = root / "dist/firmware"
    manifest_file = firmware_dir / "firmware.json"
    if not manifest_file.is_file():
        raise SystemExit(
            "Build and package firmware first: tools/build.sh build && python3 tools/package.py firmware"
        )
    manifest = json.loads(manifest_file.read_bytes())
    if manifest["version"] != (root / "VERSION").read_text().strip():
        raise SystemExit("Bundled firmware version does not match VERSION.")
    images = manifest["images"]
    expected = ["bootloader.bin", "partition-table.bin", "stickwalker.bin"]
    if sorted(image["file"] for image in images) != expected:
        raise SystemExit("Bundled firmware must include the bootloader, partition table, and application.")
    for image in images:
        path = firmware_dir / image["file"]
        if not path.is_file() or path.stat().st_size == 0:
            raise SystemExit(f"Missing or empty firmware image: {path.name}")
        if hashlib.sha256(path.read_bytes()).hexdigest() != image["sha256"]:
            raise SystemExit(f"Checksum mismatch for firmware image: {path.name}")


def desktop(a):
    if sys.platform == "darwin":
        import dmgbuild

    check_firmware()
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
            env={**os.environ, "STICKWALKER_BUNDLE_FIRMWARE": "1"},
            check=True,
        )
    version = tomllib.loads((root / "crates/sw-desktop/Cargo.toml").read_text())["package"][
        "version"
    ]
    out = root / "dist/desktop"
    out.mkdir(parents=True, exist_ok=True)
    if sys.platform == "darwin":
        app = out / "Stickwalker.app"
        if app.exists():
            shutil.rmtree(app)
        (app / "Contents/MacOS").mkdir(parents=True)
        shutil.copy2(
            root / "target" / mode / "stickwalker", app / "Contents/MacOS/stickwalker"
        )
        (app / "Contents/Resources").mkdir()
        shutil.copy2(root / "crates/sw-desktop/assets/icon.icns", app / "Contents/Resources/icon.icns")
        info = dict(
            CFBundleIconFile="icon.icns",
            CFBundleName="Stickwalker",
            CFBundleDisplayName="Stickwalker",
            CFBundleIdentifier="me.kukas.stickwalker",
            CFBundleExecutable="stickwalker",
            CFBundlePackageType="APPL",
            CFBundleVersion=version,
            CFBundleShortVersionString=version,
            NSHighResolutionCapable=True,
            LSMinimumSystemVersion="12.0",
        )
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps(info))
        if a.sign:
            identity = os.environ["STICKWALKER_SIGN_IDENTITY"]
            if not identity.startswith("Developer ID Application:"):
                raise SystemExit(
                    "Distribution requires a Developer ID Application identity."
                )
            key = os.environ["STICKWALKER_UPDATE_KEY"]
            arch = os.uname().machine
            feed = os.environ.get(
                "STICKWALKER_APPCAST",
                f"https://github.com/lumirth/stickwalker/releases/download/desktop-feed/appcast-{arch}.xml",
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
            subprocess.run(
                [
                    "xcrun",
                    "notarytool",
                    "submit",
                    str(archive),
                    "--keychain-profile",
                    os.environ["STICKWALKER_NOTARY_PROFILE"],
                    "--wait",
                ],
                check=True,
            )
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
            subprocess.run(
                [
                    "xcrun",
                    "notarytool",
                    "submit",
                    str(dmg),
                    "--keychain-profile",
                    os.environ["STICKWALKER_NOTARY_PROFILE"],
                    "--wait",
                ],
                check=True,
            )
            subprocess.run(["xcrun", "stapler", "staple", str(dmg)], check=True)
        print(dmg)
        print(app)
    elif sys.platform == "win32":
        dest = out / f"Stickwalker-{version}-windows-x86_64.exe"
        shutil.copy2(root / "target" / mode / "stickwalker.exe", dest)
        print(dest)
    else:
        archive = out / f"Stickwalker-{version}-linux-{os.uname().machine}.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(
                root / "target" / mode / "stickwalker",
                arcname="Stickwalker/stickwalker",
            )
            tar.add(root / "crates/sw-desktop/assets/icon.png", arcname="Stickwalker/icon.png")
            tar.add(root / "crates/sw-desktop/stickwalker.desktop", arcname="Stickwalker/stickwalker.desktop")
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
    app = commands.add_parser("desktop", help="build and package the native app")
    app.add_argument("--debug", action="store_true")
    app.add_argument("--no-build", action="store_true")
    app.add_argument(
        "--sign", action="store_true", help="require Developer ID and notarization"
    )
    args = parser.parse_args()
    if args.command == "firmware":
        firmware()
    elif args.command == "check-firmware":
        check_firmware()
    else:
        desktop(args)
