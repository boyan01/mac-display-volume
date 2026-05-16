# VM Driver Testing Reference

## VM Setup

Prefer the local Tart VM name `mdv-smoke`.

```sh
/opt/homebrew/bin/tart list
/opt/homebrew/bin/tart clone ghcr.io/cirruslabs/macos-tahoe-base:latest mdv-smoke
/opt/homebrew/bin/tart run --dir="mac-display-volume:/Users/yangbin/workspace/github/mac-display-volume" mdv-smoke
/opt/homebrew/bin/tart ip mdv-smoke
```

Inside the guest, the repo is mounted at:

```sh
/Volumes/My Shared Files/mac-display-volume
```

Use `tart exec` for non-interactive validation:

```sh
/opt/homebrew/bin/tart exec mdv-smoke /bin/zsh -lc 'cd "/Volumes/My Shared Files/mac-display-volume" && Scripts/vm-smoke-test.sh'
```

## Standard Smoke Test

Run this inside the VM:

```sh
cd "/Volumes/My Shared Files/mac-display-volume"
Scripts/vm-smoke-test.sh --keep-installed --require-target --set-default-virtual
```

Expected proof:

```text
DriverProbe OK
status running=false target=false priming=true queued=0 queuedMS=0.00 buffer=128 targetBuffer=0 targetBufferMS=0.00 targetIO=0 targetIOMS=0.00 dropped=0 underruns=0
```

`system_profiler SPAudioDataType` should list `Mac Display Volume`. On Tahoe VMs, `lsof -p $(pgrep coreaudiod)` may not show the bundle even when CoreAudio enumeration and `DriverProbe` pass; treat this as a warning, not a hard failure.

## Manual Install

Install both app and driver inside the VM:

```sh
cd "/Volumes/My Shared Files/mac-display-volume"
Scripts/install-local.sh
open "/Applications/Mac Display Volume.app"
.build/release/DriverProbe --mutating --configure-target --require-target --set-default-virtual --reset
```

If `DriverProbe` is stale after source changes, clean and rebuild:

```sh
swift package clean
swift build -c release --product DriverProbe
```

## Playback Tests

Prefer one longer playback for steady-state validation:

```sh
cd "/Volumes/My Shared Files/mac-display-volume"
.build/release/DriverProbe --configure-target --require-target --set-default-virtual --reset
say -o /tmp/mdv-continuous-test.aiff "Mac Display Volume continuous playback test. This is one afplay invocation, not a loop of short clips."
( afplay /tmp/mdv-continuous-test.aiff ) &
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
  .build/release/DriverProbe | grep "^status "
  sleep 0.5
done
wait
.build/release/DriverProbe | grep "^status "
```

Use short clips only to test `StartIO`/`StopIO` behavior:

```sh
cd "/Volumes/My Shared Files/mac-display-volume"
.build/release/DriverProbe --configure-target --require-target --set-default-virtual --reset
( for i in 1 2 3 4 5 6 7 8 9 10 11 12; do afplay /System/Library/Sounds/Glass.aiff; done ) &
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  .build/release/DriverProbe | grep "^status "
  sleep 0.25
done
wait
```

## Status Interpretation

`DriverProbe` prints:

```text
status running=<bool> target=<bool> priming=<bool> queued=<frames> queuedMS=<ms> buffer=<frames> targetBuffer=<frames> targetBufferMS=<ms> targetIO=<frames> targetIOMS=<ms> dropped=<frames> underruns=<count>
```

Use these rules:

- `running=true target=true priming=false` means the virtual output and relay target are active.
- `queuedMS` is the relay's current extra queued latency.
- `targetBufferMS` and `targetIOMS` estimate the selected real output device's buffer and current IOProc pull size.
- `running=false target=true` shortly after playback can be expected when the driver keeps target warm during the idle grace period.
- `running=false target=false` after the idle grace period means the real target was released.
- `dropped` should not grow during steady-state continuous playback.
- A short `afplay` loop can show state bouncing because each invocation opens and closes an audio client.
- `underruns` after playback may come from target draining when the source has already stopped; compare counters during playback before treating it as a regression.

## Recovery

If CoreAudio enumeration or `system_profiler` hangs, clean the VM state before drawing conclusions:

```sh
/opt/homebrew/bin/tart exec mdv-smoke /bin/zsh -lc 'pkill -x afplay || true; pkill -x DriverProbe || true; pkill -f "system_profiler SPAudioDataType" || true; sudo killall coreaudiod >/dev/null 2>&1 || true'
```

If the VM remains wedged, restart it:

```sh
/opt/homebrew/bin/tart stop mdv-smoke
/opt/homebrew/bin/tart run --dir="mac-display-volume:/Users/yangbin/workspace/github/mac-display-volume" mdv-smoke
```

Avoid running `AudioDeviceStart` for a physical target synchronously from HAL `StartIO`; in this repo's VM testing it caused `afplay` to fail with `AudioQueueStart failed (0x10004003)`. Keep target start off the HAL callback path and use an idle grace window to avoid repeated cold starts.
