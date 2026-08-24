#!/bin/bash
# Run all guibuxwm headless tests and report a summary.
# usage: run-all.sh
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

pass=0
fail=0
run() {
  desc=$1
  shift
  echo "=== $desc ==="
  if "$@"; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "FAILED: $desc"
  fi
  echo
}

run "workspace (ws 2)" ./run-ws-test.sh 2
run "workspace (ws 4)" ./run-ws-test.sh 4
run "effects" ./run-effects-test.sh
run "overview" ./run-overview-test.sh
run "tile mode 1 (split)" ./run-tile-test.sh 1
run "tile mode 2 (main+stack)" ./run-tile-test.sh 2
run "topbar" ./run-topbar-test.sh
run "notify" ./run-notify-test.sh
run "audio" ./run-audio-test.sh
run "topbar scroll" ./run-scroll-test.sh
run "alt+drag move" ./run-altdrag-test.sh
run "cross-monitor drag" ./run-xmondrag-test.sh
run "resize edges" ./run-resize-test.sh
run "launcher" ./run-launcher-test.sh
run "config" ./run-config-test.sh
run "primary selection" ./run-psel-test.sh
run "xwayland" ./run-xwayland-test.sh
run "restore positions" ./run-restore-test.sh

echo "=================================="
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
