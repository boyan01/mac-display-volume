#!/bin/sh
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/Driver/Build"
BUNDLE_DIR="$BUILD_DIR/MacDisplayVolumeAudio.driver"
CONTENTS_DIR="$BUNDLE_DIR/Contents"
MACOS_DIR="$CONTENTS_DIR/MacOS"
CODE_SIGN_IDENTITY="${CODE_SIGN_IDENTITY:--}"

rm -rf "$BUILD_DIR"
mkdir -p "$MACOS_DIR"

cp "$ROOT_DIR/Driver/Info.plist" "$CONTENTS_DIR/Info.plist"

clang++ \
  -std=c++20 \
  -Wall \
  -Wextra \
  -Werror \
  -fvisibility=hidden \
  -bundle \
  -framework CoreAudio \
  -framework CoreFoundation \
  "$ROOT_DIR/Driver/Sources/MacDisplayVolumeAudioDriver.cpp" \
  -o "$MACOS_DIR/MacDisplayVolumeAudio"

codesign --force --sign "$CODE_SIGN_IDENTITY" "$BUNDLE_DIR" >/dev/null

echo "$BUNDLE_DIR"
