# Release Stickwalker

Firmware and Desktop have independent versions. Set the firmware version in `VERSION` or the Desktop version in `crates/sw-desktop/Cargo.toml`. Update Cargo.lock when the crate version changes. The release workflow creates a draft titled `Stickwalker Firmware v<version>` or `Stickwalker Desktop v<version>`. Write its notes in the draft before publishing, for players and in short paragraphs: what changed and what it means for them, anything they need (such as a data-capable USB-C cable or a supported platform), and how to install. The published 0.1.0 releases are the model.

Pushes to main and pull requests run Clippy and workspace tests on macOS, Windows, and Linux. Ubuntu also runs the flash journal tests with sanitizers.

## Prepare a release

A `firmware-v*` tag builds firmware with the pinned ESP-IDF SDK and rejects a build that doesn't report exactly `VERSION`. A `desktop-v*` tag builds Desktop for Apple silicon, Windows x86-64, and Linux x86-64. Each desktop job runs the workspace tests before packaging. Both workflows create a draft release.

For the first stable launch, publish the firmware first so new installs can download it. Test the desktop installer, USB reconnection after flashing, and an app update on each supported OS. On Windows, SmartScreen can prompt when opening the unsigned executable. [Windows download instructions](README.md#windows)

## Sign the macOS release

Install a **Developer ID Application** certificate in the release Mac's login keychain. Create an App Store Connect API key with notarization access and save its credentials with `xcrun notarytool store-credentials stickwalker-release`.

```sh
cargo install gpui-updater-release --version 0.4.4 --features cli --locked
python3 -m venv build/dmg-tools
build/dmg-tools/bin/pip install -r tools/requirements-macos.txt
# Generate the signing key once and store the private half securely.
gpui-updater-release keys generate-sparkle
export STICKWALKER_UPDATE_KEY='<public half>'
export STICKWALKER_SIGN_IDENTITY='Developer ID Application: Name (TEAMID)'
export STICKWALKER_NOTARY_PROFILE=stickwalker-release
build/dmg-tools/bin/python tools/package.py desktop --sign
```

The packager signs the embedded Sparkle framework first, then the app with hardened runtime and a timestamp. It notarizes and staples both the app and its disk image.

For CI, set the repository variables `STICKWALKER_UPDATE_KEY` and `STICKWALKER_SIGN_IDENTITY`. Store the exported certificate and private key as the base64 secret `APPLE_CERTIFICATE_P12`, with its password in `APPLE_CERTIFICATE_PASSWORD`. Export it with legacy SHA-1/3DES encryption: the runner's `security import` rejects OpenSSL 3's default PKCS#12 format. Store the notarization key in `APPLE_API_KEY` and its identifiers in `APPLE_API_KEY_ID` and `APPLE_API_ISSUER_ID`. The workflow imports the credentials into the runner's keychain.

## Publish a draft

Review the artifacts and release notes, check out the tag, and run the command for that release.

```sh
python3 tools/release.py publish firmware-v0.1.0
```

Firmware publication uploads the versioned images before replacing `firmware-feed/firmware.json`. Desktop retrieves them over HTTPS and verifies their SHA-256 hashes.

```sh
export STICKWALKER_UPDATE_KEY="$(op read 'op://Developer/Stickwalker Sparkle update key/public key')"
export SPARKLE_PRIVATE_KEY="$(op read 'op://Developer/Stickwalker Sparkle update key/private key')"
python3 tools/release.py publish desktop-v0.1.0
```

Desktop publication signs and verifies the update artifacts, marks the release as GitHub's Latest, and updates `desktop-feed/latest.json` and the Sparkle appcasts. Keep the feed URLs available to installed clients and the dated preview releases available to their users.
