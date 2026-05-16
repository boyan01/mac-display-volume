# Architecture

## Goals

- Keep the display hardware volume fixed.
- Expose a macOS-controllable virtual output device with volume and mute controls.
- Relay audio to the selected real display output with bounded latency.
- Prefer dropping stale audio over accumulating seconds of delay.
- Keep the realtime audio path out of Swift.

## Components

```text
DisplayVolume.app
  SwiftUI controls
  CoreAudio device discovery
  system default-output switching
  driver custom-property reads/writes
  driver status polling

DisplayVolumeCore
  CoreAudio helpers
  driver configuration model
  driver status parser

MacDisplayVolumeAudio.driver
  AudioServerPlugIn loaded by coreaudiod
  virtual stereo Float32 output device
  software gain and mute controls
  bounded ring relay to the selected real output device
  custom properties for target, buffer, status, and reset
```

## Runtime Flow

Audio apps render into `Mac Display Volume`, the virtual HAL output device. The
driver receives that mix through `kAudioServerPlugInIOOperationWriteMix` and
stores it in an atomic ring buffer.

When the virtual device starts, the driver schedules the selected target output
device to start after a short delay. This avoids racing CoreAudio while it is
switching the system default output and stopping the previous real device. Stop
requests cancel pending starts and stop the target on the driver target queue.

The target device IOProc pulls frames from the ring buffer, applies software
volume and mute, and writes the result into the target output buffers. If the
ring does not have enough data, the driver emits silence and re-enters priming.

## Driver Boundary

The HAL driver exposes the `AudioServerPlugIn` C ABI because `coreaudiod` loads
it directly. Swift remains the right boundary for the UI and CoreAudio control
surface, but the realtime path stays in C++.

The driver must avoid blocking the audio render path. Ring-buffer state used by
IO callbacks is represented with atomics, while configuration changes and target
device start/stop are serialized outside the realtime callback path.

Standard HAL sample-rate and stream-format writes follow the
`RequestDeviceConfigurationChange` / `PerformDeviceConfigurationChange` flow.
The current driver still supports only `48 kHz`, but it now validates standard
HAL writes consistently before applying the change on the configuration-change
path.

The driver also validates IO entry points and treats late `WriteMix` cycles as
failed work instead of accepting audio that already missed the HAL output
deadline.

## Driver Custom Properties

The app talks to the driver through device-scoped custom properties:

- `tgud`: target output device UID as `CFString`.
- `bfsz`: preferred buffer frame size as a `CFPropertyList` number.
- `stat`: driver status as a comma-separated `CFString`.
- `rset`: reset relay state and health counters; written with zero data bytes.

The driver status string currently includes:

```text
running=1,target=yes,priming=no,queuedFrames=512,queuedMS=10.67,bufferFrames=128,targetBufferFrames=512,targetBufferMS=10.67,targetIOFrames=512,targetIOMS=10.67,dropped=0,underruns=0,sampleRate=48000
```

`DisplayVolume.app` polls this status so the UI can show whether the driver is
idle, priming, or running.

`queuedMS` is the relay's current extra queued latency. `targetBufferFrames` is
the selected real output device's reported buffer frame size when the target
starts, and `targetIOFrames` is the most recent frame count requested by the
target device IOProc.

## Relay State Model

`priming` means the relay is intentionally waiting for enough buffered frames
before it resumes target output. The driver enters priming on start, reset,
target changes, buffer changes, and underruns.

The preferred buffer setting is not the full relay queue size. It controls the
startup/recovery prebuffer target:

- `64` frames -> at least `512` prebuffered frames.
- `128` frames -> at least `512` prebuffered frames.
- `256` frames -> at least `1024` prebuffered frames.

The ring buffer capacity is larger than the allowed queued window. The driver
keeps the queued audio bounded and advances the read position when the writer
gets too far ahead.

## Drop And Underrun Semantics

`droppedFrames` is a playback-quality signal, not a complete accounting of every
input frame that did not reach the target.

Frames are counted as dropped when stable playback is already out of priming and
the relay must discard unread data because the writer got too far ahead. Startup
and recovery trims while `priming=yes` are not counted as dropped frames.

`underruns` increments when the target output asks for frames and the relay
cannot provide a full buffer. The driver fills the rest with silence and returns
to priming so it can rebuild a small buffer before resuming.

## Low-Latency Policy

The failure mode to avoid is unbounded buffering that turns into seconds of
delayed audio. This driver intentionally favors bounded latency:

- Keep a small prebuffer before starting or recovering relay output.
- Keep the queued window bounded.
- Trim stale queued frames instead of letting latency grow without limit.
- Emit silence and re-prime on underrun.
- Reset the ring on target, buffer, sample-rate, and manual relay reset changes.
- Keep software gain in the target output path.

The user-facing tradeoff is intentional: a short discontinuity is better than a
relay that slowly drifts into multi-second latency.

## Current Limits

- Stereo `Float32` virtual output only.
- No resampling: the selected target output must run at 48 kHz.
- The target IOProc handles common interleaved and split stereo `Float32`
  output buffers.
- Public notarized packaging is intentionally out of scope for v1.
