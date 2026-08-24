#!/bin/bash
# Restore test: verify window positions persist across a compositor restart.
# Phase 1: map a toplevel (fixed app_id), let it unmap -> compositor must
#          save the position to the state file.
# Phase 2: restart the compositor, map the same app_id -> compositor must
#          apply the saved position (log line "restore: '<app>' ->").
# The compositor log is the verdict.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
CLIENT="$ROOT/build/tests/restore-test"
APP="guibux-restore-test"

state_home=$(mktemp -d)
cfg=$(mktemp)
echo "term = true" >"$cfg"
log1=$(mktemp)
log2=$(mktemp)
trap 'rm -rf "$state_home" "$cfg" "$log1" "$log2"' EXIT

start_comp() {
  local log=$1
  GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=0 GUIBUX_TERM=true \
    XDG_STATE_HOME="$state_home" WLR_RENDERER=gles2 \
    "$COMP" -c "$cfg" >"$log" 2>&1 &
  local comp=$!
  local wd=""
  for i in $(seq 1 50); do
    wd=$(grep -oP 'WAYLAND_DISPLAY=\K\S+' "$log" | head -1)
    [ -n "$wd" ] && break
    sleep 0.1
  done
  if [ -z "$wd" ]; then
    echo "NO WAYLAND_DISPLAY"
    kill $comp 2>/dev/null
    cat "$log"
    return 2
  fi
  sleep 0.5
  WAYLAND_DISPLAY=$wd "$CLIENT"
  local rc=$?
  kill $comp 2>/dev/null
  wait $comp 2>/dev/null
  return $rc
}

echo "=== phase 1: map + unmap (save) ==="
start_comp "$log1" || { echo "FAIL: phase 1 client"; exit 1; }
sleep 0.3

state_file="$state_home/guibuxwm/window-positions"
if [ ! -f "$state_file" ]; then
  echo "FAIL: state file not created at $state_file"
  echo "--- compositor log ---"; cat "$log1"
  exit 1
fi
echo "state file contents:"
cat "$state_file"
if ! grep -q "$APP" "$state_file"; then
  echo "FAIL: app '$APP' not in state file"
  exit 1
fi
if ! grep -q "restore: saved '$APP'" "$log1"; then
  echo "FAIL: no 'restore: saved' log line in phase 1"
  echo "--- compositor log ---"; cat "$log1"
  exit 1
fi
echo "PHASE1 OK: position saved"

# move the saved position to a spot the cascade would never pick, so phase 2
# can prove the restore actually re-placed the window (not just took the path)
# the line is app|output|ws|x|y|w|h -> rewrite x,y fields explicitly
awk -F'|' -v OFS='|' -v app="$APP" '
  $1==app { $4=500; $5=300 } { print }' "$state_file" >"$state_file.tmp" \
  && mv "$state_file.tmp" "$state_file"
echo "edited state file:"
cat "$state_file"

echo "=== phase 2: restart + map (restore) ==="
start_comp "$log2" || { echo "FAIL: phase 2 client"; exit 1; }
sleep 0.3

if ! grep -q "restore: loaded 1 positions" "$log2"; then
  echo "FAIL: phase 2 did not load the saved position"
  echo "--- compositor log ---"; cat "$log2"
  exit 1
fi
if ! grep -q "restore: '$APP' ->" "$log2"; then
  echo "FAIL: no 'restore: applied' log line in phase 2"
  echo "--- compositor log ---"; cat "$log2"
  exit 1
fi
# the applied position must be the edited one (500,300), not the cascade (100,80)
if ! grep -q "restore: '$APP' -> .* 500,300 " "$log2"; then
  echo "FAIL: restored position is not the saved 500,300"
  echo "--- compositor log ---"; cat "$log2"
  exit 1
fi
echo "PHASE2 OK: position restored at saved 500,300"

echo "PASS"
exit 0
