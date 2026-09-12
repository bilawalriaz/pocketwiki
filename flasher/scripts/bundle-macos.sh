#!/bin/sh
# Assemble ESPFlashGUI.app from the SwiftPM release build.
# VERSION env overrides the bundle version (release.yml passes the git tag).
set -e
cd "$(dirname "$0")/.."

VERSION="${VERSION:-0.1}"

swift build -c release --product ESPFlashGUI

APP=build/ESPFlashGUI.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>ESPFlashGUI</string>
  <key>CFBundleIdentifier</key><string>local.pocketwiki.espflasher.gui</string>
  <key>CFBundleName</key><string>ESP Flasher</string>
  <key>CFBundleDisplayName</key><string>ESP Flasher</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

cp .build/release/ESPFlashGUI "$APP/Contents/MacOS/ESPFlashGUI"
cp -R .build/release/espflasher_ESPFlashGUI.bundle "$APP/Contents/Resources/"
codesign --force --sign - "$APP" 2>/dev/null || true
echo "Built $APP"
