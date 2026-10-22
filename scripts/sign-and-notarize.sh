#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${UFO_SIGN_IDENTITY:?Set to your Developer ID Application signing identity}"
: "${UFO_NOTARY_PROFILE:?Set to a preconfigured notarytool keychain profile name}"
[[ "$(uname -s)" == Darwin ]] || { echo 'macOS required' >&2; exit 1; }
for bundle in dist/UFO/UFO.app dist/UFO/UFO.component dist/UFO/UFO.vst3; do
  [[ -d "$bundle" ]] || { echo "Missing $bundle; run scripts/build-macos.sh first" >&2; exit 1; }
  codesign --force --options runtime --timestamp --sign "$UFO_SIGN_IDENTITY" "$bundle"
  codesign --verify --deep --strict --verbose=2 "$bundle"
done
ditto -c -k --sequesterRsrc --keepParent dist/UFO dist/UFO-notarization.zip
xcrun notarytool submit dist/UFO-notarization.zip --keychain-profile "$UFO_NOTARY_PROFILE" --wait
for bundle in dist/UFO/UFO.app dist/UFO/UFO.component dist/UFO/UFO.vst3; do
  xcrun stapler staple "$bundle"
  xcrun stapler validate "$bundle"
done
spctl --assess --type execute --verbose=2 dist/UFO/UFO.app
ditto -c -k --sequesterRsrc --keepParent dist/UFO dist/UFO-macOS-universal.zip
