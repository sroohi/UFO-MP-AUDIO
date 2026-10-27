#!/usr/bin/env bash
# Developer ID signing + notarization + stapling for "DEEJ-AY.app".
#
# Usage: packaging/sign-and-notarize.sh <path/to/DEEJ-AY.app>
# Env:   SIGNING_IDENTITY   e.g. "Developer ID Application: MP Audio (TEAMID)"
#        APPLE_ID           Apple ID e-mail used for notarization
#        APPLE_TEAM_ID      10-character team id
#        APPLE_APP_PASSWORD app-specific password for the Apple ID
set -euo pipefail

APP="${1:?path to .app required}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ENTITLEMENTS="$HERE/DeejAy.entitlements"

: "${SIGNING_IDENTITY:?SIGNING_IDENTITY not set}"
: "${APPLE_ID:?APPLE_ID not set}"
: "${APPLE_TEAM_ID:?APPLE_TEAM_ID not set}"
: "${APPLE_APP_PASSWORD:?APPLE_APP_PASSWORD not set}"

echo "==> Signing $APP"
# Sign nested code first (frameworks, helpers), then the bundle. --options runtime = hardened runtime.
find "$APP/Contents" -type f \( -name "*.dylib" -o -perm -u+x \) -not -path "*/MacOS/*" -print0 \
  | xargs -0 -I{} codesign --force --options runtime --timestamp --sign "$SIGNING_IDENTITY" "{}" || true
codesign --force --options runtime --timestamp --entitlements "$ENTITLEMENTS" --sign "$SIGNING_IDENTITY" "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"

ZIP="$(mktemp -d)/DEEJ-AY.zip"
echo "==> Notarizing"
ditto -c -k --keepParent "$APP" "$ZIP"
xcrun notarytool submit "$ZIP" --apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_PASSWORD" --wait
echo "==> Stapling"
xcrun stapler staple "$APP"
xcrun stapler validate "$APP"
spctl --assess --type execute --verbose=2 "$APP"
