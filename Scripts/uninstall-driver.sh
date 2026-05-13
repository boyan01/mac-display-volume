#!/bin/sh
set -eu

TARGET_PATH="/Library/Audio/Plug-Ins/HAL/MacDisplayVolumeAudio.driver"

sudo rm -rf "$TARGET_PATH"
sudo killall coreaudiod || true

echo "Removed $TARGET_PATH"
