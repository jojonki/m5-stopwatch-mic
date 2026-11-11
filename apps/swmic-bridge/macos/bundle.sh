#!/bin/sh
# Build "StopWatch Mic.app" (ad-hoc signed) into target/.
# Usage: apps/swmic-bridge/macos/bundle.sh
set -eu
cd "$(dirname "$0")/.."
cargo build --release
APP="target/StopWatch Mic.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp macos/Info.plist "$APP/Contents/Info.plist"
cp target/release/swmic-bridge "$APP/Contents/MacOS/swmic-bridge"
codesign --force --sign - "$APP"
echo "built: $(pwd)/$APP"
