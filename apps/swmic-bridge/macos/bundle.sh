#!/bin/sh
# Build a universal (Apple silicon + Intel) "StopWatch Mic.app" into target/
# and a release zip target/StopWatch-Mic-macOS.zip.
#
#   apps/swmic-bridge/macos/bundle.sh
#
# Signing (optional, via environment):
#   SIGN_IDENTITY   e.g. "Developer ID Application: Your Name (TEAMID)".
#                   Unset = ad-hoc signature (local use only; Gatekeeper
#                   rejects it when downloaded).
#   NOTARY_PROFILE  notarytool keychain profile (xcrun notarytool
#                   store-credentials <name> ...). When set, the app is
#                   notarized and stapled so downloads open without warnings.
set -eu
cd "$(dirname "$0")/.."

cargo build --release --target aarch64-apple-darwin
cargo build --release --target x86_64-apple-darwin

APP="target/StopWatch Mic.app"
ZIP="target/StopWatch-Mic-macOS.zip"
rm -rf "$APP" "$ZIP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp macos/Info.plist "$APP/Contents/Info.plist"
cp assets/app.icns "$APP/Contents/Resources/app.icns"
VERSION=$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)
plutil -replace CFBundleShortVersionString -string "$VERSION" "$APP/Contents/Info.plist"
lipo -create -output "$APP/Contents/MacOS/swmic-bridge" \
  target/aarch64-apple-darwin/release/swmic-bridge \
  target/x86_64-apple-darwin/release/swmic-bridge

if [ -n "${SIGN_IDENTITY:-}" ]; then
  # Hardened runtime + secure timestamp are required for notarization.
  codesign --force --options runtime --timestamp --sign "$SIGN_IDENTITY" "$APP"
else
  codesign --force --sign - "$APP"
fi
codesign --verify --strict "$APP"

ditto -c -k --norsrc --keepParent "$APP" "$ZIP"

if [ -n "${NOTARY_PROFILE:-}" ]; then
  xcrun notarytool submit "$ZIP" --keychain-profile "$NOTARY_PROFILE" --wait
  xcrun stapler staple "$APP"
  rm -f "$ZIP"
  ditto -c -k --norsrc --keepParent "$APP" "$ZIP"  # re-zip with the ticket stapled
  spctl --assess --type execute --verbose "$APP"
fi

echo "built: $(pwd)/$APP (v$VERSION)"
echo "zip:   $(pwd)/$ZIP"
