# Releasing DEEJ-AY

## What CI does
`.github/workflows/deejay-macos.yml` runs on every push and pull request touching `DeejAy/`:

1. Linux: builds the engine without JUCE and runs the test suite.
2. macOS 15 runner, universal build (arm64 + x86_64), targeting macOS 13+:
   - builds the tests and runs them natively;
   - builds the app and checks that the binary contains both architectures;
   - signs and notarizes it when the secrets below exist, and ad-hoc signs it otherwise;
   - builds a DMG and uploads it as a workflow artifact.

   On a `deejay-vX.Y.Z` tag it also creates a draft GitHub release with the DMG.

An ad-hoc signed build runs on the machine that downloads it only after the quarantine flag is cleared
(right-click > Open, or `xattr -dr com.apple.quarantine /Applications/DEEJ-AY.app`). That is fine for
testing. It is not something to hand to customers.

## Secrets for a notarized Developer ID build
| Secret | Value |
|---|---|
| `MACOS_DEVELOPER_ID_P12` | base64 of the Developer ID Application certificate + private key (`.p12`) |
| `MACOS_DEVELOPER_ID_P12_PASSWORD` | its password |
| `MACOS_SIGNING_IDENTITY` | `Developer ID Application: MP Audio (TEAMID)` |
| `APPLE_ID` | Apple ID e-mail used for notarization |
| `APPLE_TEAM_ID` | 10-character team id |
| `APPLE_APP_PASSWORD` | app-specific password for that Apple ID |

Export the certificate from Keychain Access, then `base64 -i cert.p12 | pbcopy`.

## Locally
    cmake -S DeejAy -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build --target DeejAy -j
    ./build/deejay_tests
    SIGNING_IDENTITY="Developer ID Application: MP Audio (TEAMID)" APPLE_ID=... APPLE_TEAM_ID=... APPLE_APP_PASSWORD=... \
      DeejAy/packaging/sign-and-notarize.sh build/DeejAy_artefacts/Release/DEEJ-AY.app
    DeejAy/packaging/make-dmg.sh build/DeejAy_artefacts/Release/DEEJ-AY.app 0.1.0 dist

## Entitlements
`packaging/DeejAy.entitlements` sets the hardened runtime with `com.apple.security.device.audio-input` only.

- DEEJ-AY hosts no plug-ins, so it needs neither library-validation nor JIT exceptions.
- The DDJ-FLX10's audio interface exposes an input side. The audio-input entitlement lets Core Audio open the
  device without a sandbox refusal.
- The Mac App Store is not a channel for now. Sandboxing gets in the way of user-chosen music folders and
  Serato crate import.

## Tagging a release
    git tag deejay-v0.1.0 && git push origin deejay-v0.1.0
