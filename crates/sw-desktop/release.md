# Releasing Stickwalker

Firmware and desktop use independent SemVer numbers. `VERSION` names firmware;
`crates/sw-desktop/Cargo.toml` names the desktop. Protocol v1 and SWK6 do not change merely
because either application has a patch release.

Pushes to `main` and pull requests run Clippy and headless workspace tests on
Ubuntu and Windows. Ubuntu also runs the sanitized flash journal tests.

A `firmware-v*` tag builds the firmware with the cached, pinned ESP-IDF SDK.
A `desktop-v*` tag downloads the `firmware-v<VERSION>` package once for all
three desktop builds. If that release does not exist yet, the desktop tag’s
source supplies the fallback build. A failed download or invalid package
stops packaging. macOS Apple Silicon, Windows x86-64, and Linux x86-64 each
run the workspace tests before packaging. Both release paths produce a draft
for review. Publishing a desktop release marks it as GitHub’s Latest release.

Before publishing the first desktop release, qualify its native install,
USB reset/reconnect, and self-update path on each advertised OS. A successful
cross-build is not that qualification. Intel macOS can be packaged locally but
is not included in the initial CI matrix.

## macOS signing

Use a **Developer ID Application** certificate from the Apple Developer account,
not an Apple Development certificate. Install it in the login keychain on the
release Mac. Create an App Store Connect API key with notarization access and
store it once using `xcrun notarytool store-credentials stickwalker-release`.
No Apple credentials belong in this repository.

```sh
cargo install gpui-updater-release --version 0.4.4 --features cli --locked
python3 -m venv build/dmg-tools
build/dmg-tools/bin/pip install -r tools/requirements-macos.txt
# Generate a release update key once; save its private half securely.
gpui-updater-release keys generate-sparkle
export STICKWALKER_UPDATE_KEY='<public half>'
export STICKWALKER_SIGN_IDENTITY='Developer ID Application: Name (TEAMID)'
export STICKWALKER_NOTARY_PROFILE=stickwalker-release
build/dmg-tools/bin/python tools/package.py desktop --sign
```

`--sign` embeds the pinned Sparkle framework, signs nested code inside out,
signs the application with hardened runtime and a timestamp, notarizes and
staples the app, then creates/signs/notarizes/staples its DMG. There is no
`codesign --deep` signing step. Unsigned local packages are explicitly
development artifacts; the script will not silently downgrade `--sign`.

For CI, set repository variables `STICKWALKER_UPDATE_KEY` and
`STICKWALKER_SIGN_IDENTITY`. Secrets: `APPLE_CERTIFICATE_P12` (base64 exported
Developer ID certificate + private key), `APPLE_CERTIFICATE_PASSWORD`,
`APPLE_API_KEY` (p8 contents), `APPLE_API_KEY_ID`, and `APPLE_API_ISSUER_ID`.
`tools/release.py credentials` imports them only into the disposable runner keychain.
Windows binaries use signed updater payloads, but Authenticode signing is not
configured; do not promise that Windows SmartScreen warnings are eliminated.

## Publish

For the first stable release, publish `firmware-v0.1.0` first. Then tag `desktop-v0.1.0` so Desktop embeds the published firmware. After that, bump `VERSION` for firmware changes or the desktop package version for app changes. Keep Cargo's lockfile in sync when changing a package version.

Write the player-facing notes in `docs/releases/<tag>.md` before tagging, or edit the release draft before publishing. Keep the dated candidate releases available for existing users.

After reviewing the draft artifacts, check out that release tag and run:

```sh
# Desktop updates use the same key pair used to sign the app feed.
export STICKWALKER_UPDATE_KEY='<public half>'
export SPARKLE_PRIVATE_KEY='<release signing key>'
python3 tools/release.py publish desktop-v0.1.0
# Or independently:
python3 tools/release.py publish firmware-v0.1.0
```

The desktop command downloads the reviewed artifacts, generates and verifies
Ed25519 signatures using `gpui-updater-release`, publishes the versioned
artifacts, then updates `desktop-feed/latest.json` and per-architecture Sparkle
appcasts. The firmware command publishes the versioned artifacts and updates
`firmware-feed/firmware.json` last. Firmware discovery reads that static manifest
and downloads binaries from the immutable `firmware-v<version>` release. App updates use the stable `desktop-feed`
URL. The desktop includes a firmware baseline, but neither product's version is forced to match the other's.

Firmware packages are fetched over HTTPS and each image has a SHA256 checksum.
They are **not separately cryptographically signed** in this version. Desktop
self-update payloads are signed. No eFuses or secure-boot locks are changed.
Changing the repository name requires changing both feed URLs and publishing
redirect-compatible releases for already distributed clients.
