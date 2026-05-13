# Mac Display Volume

Mac Display Volume is a small macOS virtual audio output for displays that expose
fixed HDMI/DisplayPort/USB-C audio volume to macOS.

The intended audio path is:

```text
macOS apps
  -> Mac Display Volume virtual output
  -> low-latency software gain
  -> real display audio device, such as P275MV
```

The display hardware volume stays fixed, so changing volume on macOS does not
affect the same monitor when it is later used from Windows.

## Status

This repository contains a local-use v1:

- Swift 6 + SwiftUI settings app and menu bar controls.
- CoreAudio device discovery, default-output switching, and driver configuration.
- AudioServerPlugIn HAL driver with a virtual stereo output device.
- Software volume/mute controls exposed to macOS.
- Bounded relay buffer to avoid accumulating seconds of audio delay.
- Local build/install/uninstall scripts.

The audio render path is C++ because `coreaudiod` loads AudioServerPlugIn drivers
through a C ABI. The user-facing app and configuration layer are Swift.

## Requirements

- macOS 15 or newer.
- Xcode 26 or newer.
- Swift 6.
- A valid Apple code signing identity for the local HAL driver. Set
  `CODE_SIGN_IDENTITY` if the default identity is not the one you want.

## Development

Build the SwiftUI settings app:

```sh
Scripts/build-app.sh
```

Build the HAL driver:

```sh
Scripts/build-driver.sh
```

Install locally:

```sh
Scripts/install-local.sh
```

Uninstall locally:

```sh
Scripts/uninstall-local.sh
```

After installing:

1. Open `/Applications/Mac Display Volume.app`.
2. Choose the real display audio device, for example `P275MV`.
3. Click **Apply Driver Config**.
4. Click **Use Virtual Output**.
5. Keep the monitor hardware volume fixed.

If latency ever starts accumulating, use **Reset Relay** first. If CoreAudio is
wedged, use **Restart coreaudiod** from the menu bar or settings window.

## Driver Properties

The app talks to the HAL driver through custom CoreAudio properties on the
virtual device:

- `tgud`: target output device UID, `CFString`.
- `bfsz`: preferred buffer frame size, `UInt32`.
- `stat`: diagnostic status, `CFString`.
- `rset`: reset relay command.

The driver persists `tgud` and `bfsz` through AudioServerPlugIn host storage.

## Latency Policy

The relay uses a fixed 8192-frame stereo ring buffer. When queued audio grows
past 2048 frames, the oldest frames are dropped and the driver re-anchors its
timeline. This intentionally favors a tiny discontinuity over multi-second
latency drift.

## License

Apache License 2.0.
