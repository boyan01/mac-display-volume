---
name: macos-hal-vm-test
description: Install and validate this repo's macOS CoreAudio HAL driver inside a Tart macOS VM. Use when testing Mac Display Volume driver enumeration, HAL property behavior, relay target configuration, default-output switching, app installation, afplay playback, DriverProbe status, dropped/underrun counters, or debugging VM-specific CoreAudio failures.
---

# macOS HAL VM Test

## Quick Path

Use Tart for disposable macOS validation. Prefer the existing VM `mdv-smoke` when present; otherwise create one from `ghcr.io/cirruslabs/macos-tahoe-base:latest`.

Run the repo smoke test inside the VM first:

```sh
cd "/Volumes/My Shared Files/mac-display-volume"
Scripts/vm-smoke-test.sh --keep-installed --require-target --set-default-virtual
```

Use `DriverProbe` for targeted checks:

```sh
.build/debug/DriverProbe --mutating --configure-target --require-target --set-default-virtual --reset
```

Use a single longer audio file for stable playback diagnosis; use short `afplay` loops only when explicitly testing start/stop behavior.

## Workflow

1. Confirm Tart and VM state with `/opt/homebrew/bin/tart list`.
2. Start the VM with the repo mounted as `mac-display-volume`.
3. Run `Scripts/vm-smoke-test.sh` inside the VM before manual playback tests.
4. Install the app with `Scripts/install-local.sh` only when UI behavior needs validation.
5. Set the virtual device as default output with `DriverProbe --set-default-virtual`.
6. Play audio with `afplay` and sample `DriverProbe` status during playback.
7. Treat `running=true target=true priming=false` as the main runtime proof.
8. Interpret `dropped` and `underruns` by comparing before, during, and after playback; a short `afplay` loop intentionally causes repeated `StartIO`/`StopIO`.

## Reference

Read [references/vm-driver-testing.md](references/vm-driver-testing.md) when you need exact commands, playback recipes, status interpretation, or recovery steps for stuck CoreAudio/Tart state.
