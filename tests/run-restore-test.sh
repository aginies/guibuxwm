#!/bin/bash
# Restore test: verify window positions persist across a compositor restart.
# Phase 1: map a toplevel (fixed app_id), let it unmap -> compositor must
#          save the position to the state file.
# Phase 2: restart the compositor, map the same app_id -> compositor must
#          apply the saved position (log line "restore: '<app>' ->").
# Phase 3: clean exit (quit timer) with the window still mapped -> the
#          compositor must save the position itself.
# Phase 4: saved output has the same name but a different layout box
#          (replugged monitor) -> compositor must NOT restore.
# Phase 5: saved output name does not exist -> compositor must NOT restore.
# Phase 6: app_id matches the configured terminal (term_app_id) ->
#          compositor must NOT save its position.
# The compositor log is the verdict.
# State file line: app_id|output|box_x|box_y|workspace|x|y|w|h
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
log3=$(mktemp)
log4=$(mktemp)
log5=$(mktemp)
log6=$(mktemp)
cfg_term=$(mktemp)
trap 'rm -rf "$state_home" "$cfg" "$cfg_term" "$log1" "$log2" "$log3" "$log4" "$log5" "$log6"' EXIT

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
# the line is app|output|box_x|box_y|ws|x|y|w|h -> rewrite x,y fields explicitly
awk -F'|' -v OFS='|' -v app="$APP" '
  $1==app { $6=500; $7=300 } { print }' "$state_file" >"$state_file.tmp" \
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

echo "=== phase 3: clean exit saves still-mapped windows ==="
# delete the state file: the only save that can recreate it is the
# clean-exit save (the client stays mapped and never unmaps)
rm -f "$state_file"
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=0 GUIBUX_TERM=true \
  GUIBUX_TEST_QUIT=3000 XDG_STATE_HOME="$state_home" WLR_RENDERER=gles2 \
  "$COMP" -c "$cfg" >"$log3" 2>&1 &
comp3=$!
wd=""
for i in $(seq 1 50); do
  wd=$(grep -oP 'WAYLAND_DISPLAY=\K\S+' "$log3" | head -1)
  [ -n "$wd" ] && break
  sleep 0.1
done
if [ -z "$wd" ]; then
  echo "NO WAYLAND_DISPLAY (phase 3)"
  kill $comp3 2>/dev/null
  cat "$log3"
  exit 1
fi
sleep 0.5
WAYLAND_DISPLAY=$wd "$CLIENT" keep &
client3=$!
# the compositor exits on its own via the quit timer (clean exit)
for i in $(seq 1 50); do
  kill -0 $comp3 2>/dev/null || break
  sleep 0.1
done
if kill -0 $comp3 2>/dev/null; then
  echo "FAIL: compositor did not exit on its own"
  cat "$log3"
  kill $comp3 2>/dev/null
  kill $client3 2>/dev/null
  exit 1
fi
kill $client3 2>/dev/null
wait $client3 2>/dev/null

if [ ! -f "$state_file" ]; then
  echo "FAIL: state file not recreated on clean exit"
  echo "--- compositor log ---"; cat "$log3"
  exit 1
fi
echo "state file contents:"
cat "$state_file"
if ! grep -q "$APP" "$state_file"; then
  echo "FAIL: app '$APP' not in state file after clean exit"
  exit 1
fi
if ! grep -q "restore: saved" "$log3"; then
  echo "FAIL: no 'restore: saved' log line on clean exit"
  echo "--- compositor log ---"; cat "$log3"
  exit 1
fi
echo "PHASE3 OK: position saved on clean exit"

echo "=== phase 4: replugged monitor (same name, new box) is not restored ==="
# the headless output sits at box 0,0; claim the saved position came from a
# monitor at 500,500 -> name matches, box does not -> normal placement
awk -F'|' -v OFS='|' -v app="$APP" '
  $1==app { $3=500; $4=500; $6=500; $7=300 } { print }' "$state_file" \
  >"$state_file.tmp" && mv "$state_file.tmp" "$state_file"
echo "edited state file:"
cat "$state_file"
start_comp "$log4" || { echo "FAIL: phase 4 client"; exit 1; }
sleep 0.3
if ! grep -q "output 'HEADLESS-1' moved" "$log4"; then
  echo "FAIL: no 'output moved' log line in phase 4"
  echo "--- compositor log ---"; cat "$log4"
  exit 1
fi
if grep -q "restore: '$APP' ->" "$log4"; then
  echo "FAIL: position was restored onto the replugged monitor"
  echo "--- compositor log ---"; cat "$log4"
  exit 1
fi
# the window took the cascade (100,80), and the unmap save wrote it back
if ! grep -q "^$APP|HEADLESS-1|0|0|1|100|80|" "$state_file"; then
  echo "FAIL: window was not placed at the cascade position"
  cat "$state_file"
  exit 1
fi
echo "PHASE4 OK: replugged monitor skipped, cascade placement"

echo "=== phase 5: missing monitor is not restored ==="
awk -F'|' -v OFS='|' -v app="$APP" '
  $1==app { $2="NOPE-1" } { print }' "$state_file" \
  >"$state_file.tmp" && mv "$state_file.tmp" "$state_file"
echo "edited state file:"
cat "$state_file"
start_comp "$log5" || { echo "FAIL: phase 5 client"; exit 1; }
sleep 0.3
if ! grep -q "missing output 'NOPE-1'" "$log5"; then
  echo "FAIL: no 'missing output' log line in phase 5"
  echo "--- compositor log ---"; cat "$log5"
  exit 1
fi
if grep -q "restore: '$APP' ->" "$log5"; then
  echo "FAIL: position was restored onto a missing monitor"
  echo "--- compositor log ---"; cat "$log5"
  exit 1
fi
echo "PHASE5 OK: missing monitor skipped, cascade placement"

echo "=== phase 6: configured terminal (term_app_id) is not saved ==="
rm -f "$state_file"
echo "term = some-other-terminal" >"$cfg_term"
echo "term_app_id = $APP" >>"$cfg_term"
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=0 GUIBUX_TERM=true \
  XDG_STATE_HOME="$state_home" WLR_RENDERER=gles2 \
  "$COMP" -c "$cfg_term" >"$log6" 2>&1 &
comp6=$!
wd=""
for i in $(seq 1 50); do
  wd=$(grep -oP 'WAYLAND_DISPLAY=\K\S+' "$log6" | head -1)
  [ -n "$wd" ] && break
  sleep 0.1
done
if [ -z "$wd" ]; then
  echo "NO WAYLAND_DISPLAY (phase 6)"
  kill $comp6 2>/dev/null
  cat "$log6"
  exit 1
fi
sleep 0.5
WAYLAND_DISPLAY=$wd "$CLIENT"
rc6=$?
kill $comp6 2>/dev/null
wait $comp6 2>/dev/null
if [ $rc6 -ne 0 ]; then
  echo "FAIL: phase 6 client"
  exit 1
fi
sleep 0.3
if ! grep -q "restore: terminal app_id '$APP' (term_app_id)" "$log6"; then
  echo "FAIL: term_app_id was not picked up"
  echo "--- compositor log ---"; cat "$log6"
  exit 1
fi
if grep -q "restore: saved '$APP'" "$log6"; then
  echo "FAIL: terminal position was saved"
  echo "--- compositor log ---"; cat "$log6"
  exit 1
fi
if [ -f "$state_file" ] && grep -q "$APP" "$state_file"; then
  echo "FAIL: terminal entry present in state file"
  cat "$state_file"
  exit 1
fi
echo "PHASE6 OK: terminal excluded from restore"

echo "PASS"
exit 0
