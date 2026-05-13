# Mac Display Volume

[中文文档](README.zh-CN.md)

Mac Display Volume lets macOS control an external display's audio volume like a
normal speaker.

Some displays expose HDMI, DisplayPort, or USB-C audio to macOS as a fixed-volume
output device. The keyboard volume keys and menu bar volume slider may be
disabled, or they may change the display's own hardware volume. If the same
display is also used with Windows, a game console, or another device, that shared
hardware volume can become annoying quickly.

Mac Display Volume creates a virtual audio output device on the Mac:

```text
macOS apps
  -> Mac Display Volume
  -> software volume control
  -> your real display speaker
```

Set the system output to `Mac Display Volume`, then choose the real display audio
device as its target. macOS volume changes are applied in software before the
audio is forwarded to the display. The display's own hardware volume can stay
fixed, so changing volume on macOS does not affect the same monitor when it is
used from another machine.

## Who It Is For

- You use an external display with built-in speakers or audio output.
- macOS cannot adjust that display's volume normally.
- You want volume changes to affect only the current Mac.
- The same display is also used by Windows, a game console, or another device.

## Features

- Creates a macOS virtual audio output device.
- Supports system volume and mute controls.
- Forwards audio to the real display audio device with low latency.
- Provides a menu bar app for choosing the target output device.
- Can switch the default output to the virtual device.
- Keeps the display's hardware volume unchanged.

## Requirements

- macOS 15 or newer.
- Apple Silicon Mac.
- An external display with audio output.

## Installation

The current version is intended for local build and installation. Xcode 26 or
newer is required.

```sh
Scripts/install-local.sh
```

The install script builds the app and HAL driver, then installs them to:

- `/Applications/Mac Display Volume.app`
- `/Library/Audio/Plug-Ins/HAL/MacDisplayVolumeAudio.driver`

You may be asked for an administrator password during installation. If the new
audio device does not appear immediately after installation, restart CoreAudio.
If it still does not appear, rebooting the Mac usually clears stale CoreAudio
caches and old driver helper processes.

## Usage

1. Open `/Applications/Mac Display Volume.app`.
2. Choose the real display audio device, for example `P275MV`.
3. Click **Apply Driver Config**.
4. Click **Use Virtual Output**.
5. Keep the display's own hardware volume fixed.

After that, use the keyboard volume keys, the menu bar volume slider, or System
Settings to control the volume of `Mac Display Volume`.

## Notes

- The target output device must support `48 kHz`.
- Do not set `Mac Display Volume` itself as the target device.
- If audio latency starts accumulating, use **Reset Relay** first.
- If CoreAudio gets into a bad state, use **Restart coreaudiod** or reboot the
  Mac.

## Driver Smoke Test

Run the HAL driver smoke test inside a disposable macOS VM:

```sh
Scripts/vm-smoke-test.sh
```

The script builds and installs the driver, restarts `coreaudiod`, verifies
`system_profiler` enumeration and `coreaudiod` bundle loading, runs
`DriverProbe`, and uninstalls the driver by default.

## Uninstall

```sh
Scripts/uninstall-local.sh
```

The uninstall script removes the app and HAL driver, then restarts CoreAudio.

## How It Works

Mac Display Volume has two parts:

- SwiftUI menu bar app: selects the target device, applies configuration, and
  switches the default output.
- CoreAudio HAL driver: exposes the virtual output device, applies software
  volume, and forwards audio.

The driver uses a fixed-size relay buffer. When too much audio is queued, it
drops the oldest frames and re-anchors the timeline to avoid accumulating
multi-second latency.

## License

Apache License 2.0.
