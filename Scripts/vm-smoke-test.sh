#!/bin/sh
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DRIVER_TARGET="/Library/Audio/Plug-Ins/HAL/MacDisplayVolumeAudio.driver"
REPORT_DIR="${TMPDIR:-/tmp}/mac-display-volume-smoke"
SYSTEM_PROFILER_REPORT="$REPORT_DIR/system_profiler_audio.txt"
LSOF_REPORT="$REPORT_DIR/coreaudiod_lsof.txt"
KEEP_INSTALLED=0
REQUIRE_TARGET=0
SET_DEFAULT_VIRTUAL=0

usage() {
  cat <<'EOF'
Usage: Scripts/vm-smoke-test.sh [--keep-installed] [--require-target] [--set-default-virtual]

Run this inside a disposable macOS VM. The script installs the HAL driver,
restarts coreaudiod, verifies CoreAudio enumeration/loading, runs DriverProbe,
and uninstalls the driver unless --keep-installed is passed.

--keep-installed       Leave the driver installed after the smoke test.
--require-target       Fail unless a non-virtual 48 kHz output target exists.
--set-default-virtual  Set Mac Display Volume as the guest default output.
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --keep-installed)
      KEEP_INSTALLED=1
      ;;
    --require-target)
      REQUIRE_TARGET=1
      ;;
    --set-default-virtual)
      SET_DEFAULT_VIRTUAL=1
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

log() {
  printf '[vm-smoke] %s\n' "$*"
}

fail() {
  printf '[vm-smoke] failed: %s\n' "$*" >&2
  exit 1
}

restart_coreaudiod() {
  sudo killall coreaudiod >/dev/null 2>&1 || true
  sleep 3
}

uninstall_driver() {
  sudo rm -rf "$DRIVER_TARGET"
  restart_coreaudiod
}

cleanup() {
  if [ "$KEEP_INSTALLED" -eq 0 ]; then
    log "uninstalling driver"
    uninstall_driver
  fi
}

trap cleanup EXIT INT TERM

if [ "$(uname -s)" != "Darwin" ]; then
  fail "this smoke test must run on macOS"
fi

mkdir -p "$REPORT_DIR"

log "building driver"
DRIVER_PATH="$("$ROOT_DIR/Scripts/build-driver.sh")"

log "building DriverProbe"
swift build --package-path "$ROOT_DIR" --product DriverProbe >/dev/null
PROBE="$ROOT_DIR/.build/debug/DriverProbe"

log "installing driver into $DRIVER_TARGET"
sudo rm -rf "$DRIVER_TARGET"
sudo ditto "$DRIVER_PATH" "$DRIVER_TARGET"
sudo chown -R root:wheel "$DRIVER_TARGET"
restart_coreaudiod

log "checking system_profiler enumeration"
for attempt in 1 2 3 4 5 6 7 8 9 10; do
  system_profiler SPAudioDataType > "$SYSTEM_PROFILER_REPORT"
  if grep -q "Mac Display Volume" "$SYSTEM_PROFILER_REPORT"; then
    break
  fi
  if [ "$attempt" -eq 10 ]; then
    tail -n 80 "$SYSTEM_PROFILER_REPORT" >&2
    fail "Mac Display Volume was not listed by system_profiler"
  fi
  sleep 3
done

log "checking coreaudiod loaded the bundle"
COREAUDIOD_PID="$(pgrep -x coreaudiod | head -n 1 || true)"
if [ -z "$COREAUDIOD_PID" ]; then
  fail "coreaudiod is not running"
fi
sudo lsof -p "$COREAUDIOD_PID" > "$LSOF_REPORT"
if ! grep -q "MacDisplayVolumeAudio" "$LSOF_REPORT"; then
  log "warning: coreaudiod lsof did not show MacDisplayVolumeAudio; continuing with DriverProbe"
fi

PROBE_ARGS="--mutating --configure-target"
if [ "$REQUIRE_TARGET" -eq 1 ]; then
  PROBE_ARGS="$PROBE_ARGS --require-target"
fi
if [ "$SET_DEFAULT_VIRTUAL" -eq 1 ]; then
  PROBE_ARGS="$PROBE_ARGS --set-default-virtual"
fi

log "running DriverProbe"
# shellcheck disable=SC2086
"$PROBE" $PROBE_ARGS

log "smoke test passed"
