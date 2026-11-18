#!/usr/bin/env python3
"""Publish a reviewed draft, or import Apple credentials on an ephemeral CI runner."""

import argparse
import base64
import os
from pathlib import Path
import secrets
import subprocess
import tempfile
import tomllib

root = Path(__file__).resolve().parents[1]


def run(*args):
    subprocess.run(args, cwd=root, check=True)


def make_feeds(artifacts, out):
    version = tomllib.loads((root / "crates/sw-desktop/Cargo.toml").read_text())["package"][
        "version"
    ]
    # The release library verifies each generated signature with this public key.
    os.environ["SPARKLE_PUBLIC_KEY"] = os.environ["STICKWALKER_UPDATE_KEY"]
    if not os.environ.get("SPARKLE_PRIVATE_KEY"):
        raise SystemExit("Set SPARKLE_PRIVATE_KEY from the release signing key.")
    out.mkdir(parents=True, exist_ok=True)
    base = "https://github.com/lumirth/stickwalker/releases/download"
    layout = [
        "--base-url",
        base,
        "--release-dir",
        "desktop-v{version}",
        "--channel-dir",
        "desktop-feed",
        "--tag",
        "v" + version,
        "--dist",
        str(artifacts),
    ]
    subprocess.run(
        [
            "gpui-updater-release",
            "manifest",
            *layout,
            "--out",
            str(out / "latest.json"),
            "--app-id",
            "me.kukas.stickwalker",
            "--signatures",
            "ignore",
            "--ed25519-key-env",
            "SPARKLE_PRIVATE_KEY",
        ],
        check=True,
    )
    subprocess.run(
        [
            "gpui-updater-release",
            "appcast",
            *layout,
            "--out-dir",
            str(out),
            "--title",
            "Stickwalker",
            "--minimum-system-version",
            "12.0",
            "--no-deltas",
        ],
        check=True,
    )
    print(out)


def credentials():
    with tempfile.TemporaryDirectory() as temp:
        folder = Path(temp)
        certificate = folder / "developer-id.p12"
        certificate.write_bytes(base64.b64decode(os.environ["APPLE_CERTIFICATE_P12"]))
        key = folder / "AuthKey.p8"
        key.write_text(os.environ["APPLE_API_KEY"])
        keychain = str(Path(os.environ["RUNNER_TEMP"]) / "stickwalker.keychain-db")
        password = secrets.token_urlsafe(32)
        run("security", "create-keychain", "-p", password, keychain)
        run("security", "set-keychain-settings", "-lut", "21600", keychain)
        run("security", "unlock-keychain", "-p", password, keychain)
        run(
            "security",
            "import",
            str(certificate),
            "-P",
            os.environ["APPLE_CERTIFICATE_PASSWORD"],
            "-k",
            keychain,
            "-T",
            "/usr/bin/codesign",
        )
        run(
            "security",
            "set-key-partition-list",
            "-S",
            "apple-tool:,apple:,codesign:",
            "-s",
            "-k",
            password,
            keychain,
        )
        run("security", "list-keychains", "-d", "user", "-s", keychain)
        run(
            "xcrun",
            "notarytool",
            "store-credentials",
            "stickwalker-release",
            "--key",
            str(key),
            "--key-id",
            os.environ["APPLE_API_KEY_ID"],
            "--issuer",
            os.environ["APPLE_API_ISSUER_ID"],
        )


def publish(tag):
    with tempfile.TemporaryDirectory(prefix="stickwalker-release-") as temp:
        folder = Path(temp)
        if tag.startswith("desktop-v"):
            version = tomllib.loads((root / "crates/sw-desktop/Cargo.toml").read_text())[
                "package"
            ]["version"]
            if tag != "desktop-v" + version:
                raise SystemExit("Check out the desktop release tag before publishing.")
            assets, feeds = folder / "assets", folder / "feeds"
            run("gh", "release", "download", tag, "--dir", str(assets))
            make_feeds(assets, feeds)
            run("gh", "release", "upload", tag, *map(str, feeds.iterdir()), "--clobber")
            run("gh", "release", "edit", tag, "--draft=false", "--latest")
            exists = (
                subprocess.run(
                    ["gh", "release", "view", "desktop-feed"],
                    cwd=root,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                ).returncode
                == 0
            )
            if exists:
                run(
                    "gh",
                    "release",
                    "upload",
                    "desktop-feed",
                    *map(str, feeds.iterdir()),
                    "--clobber",
                )
            else:
                run(
                    "gh",
                    "release",
                    "create",
                    "desktop-feed",
                    *map(str, feeds.iterdir()),
                    "--prerelease",
                    "--latest=false",
                    "--title",
                    "Desktop update feeds",
                    "--notes",
                    "Stable desktop update metadata. Download the versioned desktop release to install.",
                )
        elif tag == "firmware-v" + (root / "VERSION").read_text().strip():
            assets = folder / "firmware"
            run("gh", "release", "download", tag, "--dir", str(assets))
            run("gh", "release", "edit", tag, "--draft=false", "--latest=false")
            exists = subprocess.run(["gh", "release", "view", "firmware-feed"], cwd=root,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
            if not exists:
                run("gh", "release", "create", "firmware-feed", "--prerelease", "--latest=false",
                    "--title", "Firmware update feed", "--notes", "Current firmware metadata and installation images.")
            # Payloads live in the immutable versioned release.
            run("gh", "release", "upload", "firmware-feed", str(assets / "firmware.json"), "--clobber")
        else:
            raise SystemExit(
                "Use a firmware-v or desktop-v tag matching this checkout."
            )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("credentials")
    publish_command = commands.add_parser("publish")
    publish_command.add_argument("tag")
    args = parser.parse_args()
    if args.command == "credentials":
        credentials()
    else:
        publish(args.tag)
