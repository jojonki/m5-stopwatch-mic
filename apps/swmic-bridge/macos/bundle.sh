#!/bin/sh
# Build a universal (Apple silicon + Intel), ad-hoc signed "StopWatch Mic.app"
# into target/. Usage: apps/swmic-bridge/macos/bundle.sh
set -eu
cd "$(dirname "$0")/.."
cargo build --release --target aarch64-apple-darwin
cargo build --release --target x86_64-apple-darwin
APP="target/StopWatch Mic.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp macos/Info.plist "$APP/Contents/Info.plist"
VERSION=$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)
plutil -replace CFBundleShortVersionString -string "$VERSION" "$APP/Contents/Info.plist"
lipo -create -output "$APP/Contents/MacOS/swmic-bridge" \
  target/aarch64-apple-darwin/release/swmic-bridge \
  target/x86_64-apple-darwin/release/swmic-bridge
codesign --force --sign - "$APP"
echo "built: $(pwd)/$APP"
