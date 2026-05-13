#!/bin/sh
set -eu

APP_TARGET="/Applications/Mac Display Volume.app"
DRIVER_TARGET="/Library/Audio/Plug-Ins/HAL/MacDisplayVolumeAudio.driver"

pkill -x DisplayVolume || true
rm -rf "$APP_TARGET"
sudo rm -rf "$DRIVER_TARGET"
sudo killall coreaudiod || true

echo "Removed $APP_TARGET"
echo "Removed $DRIVER_TARGET"
