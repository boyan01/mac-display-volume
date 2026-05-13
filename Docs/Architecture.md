# Architecture

## Goals

- Keep the display hardware volume fixed.
- Expose a macOS-controllable virtual output device with volume and mute controls.
- Relay audio to the selected real display output with bounded latency.
- Prefer dropping stale audio over preserving every frame when the relay falls behind.

## Components

```text
DisplayVolume.app
  SwiftUI settings
  CoreAudio device discovery
  target-device configuration

MacDisplayVolumeAudio.driver
  AudioServerPlugIn loaded by coreaudiod
  virtual stereo output device
  software gain and mute controls
  bounded relay to real output device
```

## Low-Latency Policy

The old `proxy-audio-device` failure mode we observed is seconds of accumulated
audio delay. This implementation should avoid unbounded buffering:

- Keep a small target latency, initially 64-128 frames at 48 kHz.
- Track input and output host time continuously.
- If buffered audio exceeds the target window, discard the oldest frames.
- On underrun, emit silence and re-anchor timing instead of drifting.
- On sample-rate or output-device change, reset the ring buffer immediately.
- Keep volume gain inside the render path, not in a secondary processing queue.

The user-facing tradeoff is intentional: a brief discontinuity is better than a
relay that slowly drifts into multi-second latency.

## Driver Boundary

The HAL driver must expose a C ABI because `coreaudiod` loads it through
`AudioServerPlugIn`. Swift remains appropriate for the app and configuration
surface, but the audio render path should stay in C++ for predictable ABI and
real-time behavior.

## Current Limits

- Stereo Float32 only.
- No resampling: v1 expects the target display output to run at 48 kHz.
- The target IOProc handles common interleaved and split stereo Float32 buffers.
- Public notarized packaging is intentionally out of scope for v1.
