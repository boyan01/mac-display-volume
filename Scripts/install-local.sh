#!/bin/sh
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APP_PATH="$("$ROOT_DIR/Scripts/build-app.sh")"
DRIVER_PATH="$("$ROOT_DIR/Scripts/build-driver.sh")"
APP_TARGET="/Applications/Mac Display Volume.app"
DRIVER_TARGET="/Library/Audio/Plug-Ins/HAL/MacDisplayVolumeAudio.driver"

pkill -x DisplayVolume || true
rm -rf "$APP_TARGET"
ditto "$APP_PATH" "$APP_TARGET"

sudo rm -rf "$DRIVER_TARGET"
sudo ditto "$DRIVER_PATH" "$DRIVER_TARGET"
sudo chown -R root:wheel "$DRIVER_TARGET"
sudo killall coreaudiod || true

echo "Installed $APP_TARGET"
echo "Installed $DRIVER_TARGET"
