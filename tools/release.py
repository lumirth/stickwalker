#!/usr/bin/env python3
"""Publish a reviewed draft, or import Apple credentials on an ephemeral CI runner."""

import argparse
import base64
import os
from pathlib import Path
import secrets
import subprocess
import tempfile

from package import APPCAST_NAME, APP_ID, FIRMWARE_IMAGES, RELEASE_URL, check_firmware, desktop_version, update_key, root


def run(*args):
    subprocess.run(args, cwd=root, check=True)


def desktop_assets(version):
    return [
        f"Stickwalker-{version}-{platform}"
        for platform in ("macos-arm64.dmg", "windows-x86_64.exe", "linux-x86_64.tar.gz")
    ]


def make_feeds(artifacts, out):
    version = desktop_version()
    for name in desktop_assets(version):
        path = artifacts / name
        if not path.is_file() or path.stat().st_size == 0:
            raise SystemExit(f"Missing desktop installer: {name}")
    # The release library verifies each generated signature with this public key.
    signing_env = {**os.environ, "SPARKLE_PUBLIC_KEY": update_key()}
    if not os.environ.get("SPARKLE_PRIVATE_KEY"):
        raise SystemExit("Set SPARKLE_PRIVATE_KEY from the release signing key.")
    out.mkdir(parents=True, exist_ok=True)
    layout = [
        "--base-url",
        RELEASE_URL,
        "--appcast-name", APPCAST_NAME,
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
            APP_ID,
            "--signatures",
            "ignore",
            "--require", "macos",
            "--require", "windows",
            "--require", "linux",
            "--ed25519-key-env",
            "SPARKLE_PRIVATE_KEY",
        ],
        env=signing_env,
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
        env=signing_env,
        check=True,
    )
    for archive in artifacts.glob("Stickwalker-*-macos-*.dmg"):
        arch = archive.stem.rsplit("-", 1)[1]
        feed = out / APPCAST_NAME.format(arch=arch)
        if not feed.is_file() or archive.name not in feed.read_text():
            raise SystemExit(f"Missing Sparkle feed for {archive.name}.")
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
        run("security", "default-keychain", "-d", "user", "-s", keychain)
        run(
            "xcrun",
            "notarytool",
            "store-credentials",
            "stickwalker-release",
            "--keychain", keychain,
            "--key",
            str(key),
            "--key-id",
            os.environ["APPLE_API_KEY_ID"],
            "--issuer",
            os.environ["APPLE_API_ISSUER_ID"],
        )
        with Path(os.environ["GITHUB_ENV"]).open("a") as env:
            env.write(f"STICKWALKER_NOTARY_KEYCHAIN={keychain}\n")


def publish(tag):
    with tempfile.TemporaryDirectory(prefix="stickwalker-release-") as temp:
        folder = Path(temp)
        if tag.startswith("desktop-v"):
            version = desktop_version()
            if tag != "desktop-v" + version:
                raise SystemExit("Check out the desktop release tag before publishing.")
            assets, feeds = folder / "assets", folder / "feeds"
            patterns = [arg for name in desktop_assets(version) for arg in ("--pattern", name)]
            run("gh", "release", "download", tag, "--dir", str(assets), *patterns)
            make_feeds(assets, feeds)
            run("gh", "release", "upload", tag, *map(str, feeds.iterdir()), "--clobber")
            # The feed must point to public installers before we advertise it.
            draft = subprocess.check_output(
                ["gh", "release", "view", tag, "--json", "isDraft", "--jq", ".isDraft"],
                cwd=root, text=True,
            ).strip()
            if draft == "true":
                run("gh", "release", "edit", tag, "--draft=false", "--latest=false")
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
            run("gh", "release", "edit", tag, "--latest")
        elif tag == "firmware-v" + (root / "VERSION").read_text().strip():
            assets = folder / "firmware"
            patterns = [arg for name in ("firmware.json", *FIRMWARE_IMAGES) for arg in ("--pattern", name)]
            run("gh", "release", "download", tag, "--dir", str(assets), *patterns)
            check_firmware(assets, release=True)
            run("gh", "release", "edit", tag, "--draft=false", "--latest=false")
            exists = subprocess.run(["gh", "release", "view", "firmware-feed"], cwd=root,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
            if not exists:
                run("gh", "release", "create", "firmware-feed", "--prerelease", "--latest=false",
                    "--title", "Firmware update feed", "--notes", "Latest Stickwalker firmware manifest.")
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
