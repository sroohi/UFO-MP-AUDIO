#!/usr/bin/env bash
# Builds a compressed DMG with an Applications symlink. If signing env vars are present the DMG is
# signed, notarized and stapled as well (Gatekeeper checks the disk image, not only the app).
#
# Usage: packaging/make-dmg.sh <path/to/DEEJ-AY.app> <version> [output-dir]
set -euo pipefail

APP="${1:?path to .app required}"
VERSION="${2:?version required}"
OUT="${3:-dist}"
NAME="DEEJ-AY-${VERSION}"
STAGE="$(mktemp -d)"

mkdir -p "$OUT"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
cp "$(dirname "$0")/../THIRD-PARTY-NOTICES.md" "$STAGE/Third-party notices.md"

hdiutil create -volname "DEEJ-AY" -srcfolder "$STAGE" -ov -format UDZO -fs HFS+ "$OUT/$NAME.dmg"

if [[ -n "${SIGNING_IDENTITY:-}" && -n "${APPLE_ID:-}" ]]; then
  codesign --force --timestamp --sign "$SIGNING_IDENTITY" "$OUT/$NAME.dmg"
  xcrun notarytool submit "$OUT/$NAME.dmg" --apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_PASSWORD" --wait
  xcrun stapler staple "$OUT/$NAME.dmg"
fi
shasum -a 256 "$OUT/$NAME.dmg" | tee "$OUT/$NAME.dmg.sha256"
