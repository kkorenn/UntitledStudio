#!/bin/bash
# Builds a Release .app and packs it into dist/UntitledStudio-<version>-macOS.dmg.
# Ad-hoc signed by default. Set DEVELOPER_ID_APP (+ APPLE_ID, TEAM_ID, APP_PASSWORD) for a real,
# notarized build; see RELEASING.md.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^project(UntitledStudio VERSION \([0-9.]*\)).*/\1/p' CMakeLists.txt)
APP_NAME="Untitled Studio"
DMG="dist/UntitledStudio-$VERSION-macOS.dmg"
STAGE=dist/stage

cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release

rm -rf "$STAGE" "$DMG"
mkdir -p "$STAGE"
cp -R "build-release/UntitledStudio_artefacts/Release/$APP_NAME.app" "$STAGE/"
cp LICENSE "$STAGE/LICENSE.txt"
ln -s /Applications "$STAGE/Applications"
APP="$STAGE/$APP_NAME.app"

if [[ -z "${DEVELOPER_ID_APP:-}" ]]; then
    echo "== Ad-hoc signing (DEVELOPER_ID_APP not set: users will need right-click > Open the first time)"
    codesign --force --deep -s - "$APP"
else
    # ---- Optional: real signing + notarization (Apple Developer Program) ----
    echo "== Signing with '$DEVELOPER_ID_APP' (hardened runtime)"
    codesign --force --deep --options runtime --timestamp \
             --entitlements resources/entitlements.plist -s "$DEVELOPER_ID_APP" "$APP"
fi
codesign --verify --deep --strict "$APP"

hdiutil create -volname "$APP_NAME $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
rm -rf "$STAGE"

if [[ -n "${DEVELOPER_ID_APP:-}" ]]; then
    codesign --force --timestamp -s "$DEVELOPER_ID_APP" "$DMG"
    if [[ -n "${APPLE_ID:-}" && -n "${TEAM_ID:-}" && -n "${APP_PASSWORD:-}" ]]; then
        echo "== Notarizing (takes a few minutes)"
        xcrun notarytool submit "$DMG" --apple-id "$APPLE_ID" --team-id "$TEAM_ID" --password "$APP_PASSWORD" --wait
        xcrun stapler staple "$DMG"
    else
        echo "== Skipping notarization (APPLE_ID / TEAM_ID / APP_PASSWORD not set)"
    fi
fi

echo "== $DMG ($(du -h "$DMG" | cut -f1))"
