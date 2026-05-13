#!/bin/sh
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DRIVER_PATH="$("$ROOT_DIR/Scripts/build-driver.sh")"
TARGET_PATH="/Library/Audio/Plug-Ins/HAL/MacDisplayVolumeAudio.driver"

sudo rm -rf "$TARGET_PATH"
sudo ditto "$DRIVER_PATH" "$TARGET_PATH"
sudo chown -R root:wheel "$TARGET_PATH"
sudo killall coreaudiod || true

echo "Installed $TARGET_PATH"
