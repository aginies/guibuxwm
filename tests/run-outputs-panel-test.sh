#!/bin/bash
# Outputs panel test: the in-compositor layout editor (Mod+m) must show
# every connected output (enabled or disabled) and apply each key live:
# move right/left, cycle mode, rotate, disable, close. Headless with 2
# outputs (HEADLESS-1/2) and with 3 outputs (HEADLESS-1/2/3, the 3-output
# run verifies the row reflow by effective, rotation-aware widths); the
# compositor's GUIBUX_TEST_OUTPUTS_PANEL hook is the verdict.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
fail=0

# case_run <desc> <extra outputs N> <config line> <ok marker>
case_run() {
  desc=$1
  n=$2
  cfg=$(mktemp)
  echo "$3" >"$cfg"
  log=$(mktemp)
  state_home=$(mktemp -d)
  env GUIBUX_TEST_EXTRA_OUTPUTS=$n GUIBUX_TEST_OUTPUTS_PANEL=1 \
      WLR_RENDERER=vulkan XDG_STATE_HOME=$state_home GUIBUX_CONFIG=$cfg \
      "$COMP" >"$log" 2>&1 &
  comp=$!
  sleep 6
  kill $comp 2>/dev/null
  wait $comp 2>/dev/null
  if grep -q "$4" "$log" && ! grep -q "outputs-panel-test: FAIL" "$log"; then
    echo "outputs-panel-test: OK ($desc)"
    rm -f "$log" "$cfg"
    rm -rf "$state_home"
  else
    echo "outputs-panel-test: FAILED ($desc)"
    cat "$log"
    rm -f "$log" "$cfg"
    rm -rf "$state_home"
    fail=1
  fi
}

case_run "2 outputs" 1 "outputs = HEADLESS-1@0x0,HEADLESS-2@1280x0" \
  "outputs-panel-test: OK (show, move, mode, transform, disable, close)"
case_run "3 outputs reflow" 2 \
  "outputs = HEADLESS-1@0x0:90,HEADLESS-2@1280x0,HEADLESS-3@2560x0" \
  "outputs-panel-test: OK3 (3-monitor reflow, close)"

[ "$fail" -eq 0 ]
