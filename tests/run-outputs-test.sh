#!/bin/bash
# Output arrangement test: a bogus config name must not kill the session
# (auto-arrange), manual placement boxes apply, NAME@off disables a
# monitor, and unplugging an output rehomes its windows. Headless with 2
# outputs (HEADLESS-1/2); the compositor's GUIBUX_TEST_OUTPUTS hook is
# the verdict.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
CLIENT="$ROOT/build/tests/ws-test"
fail=0

# case_run <desc> <GUIBUX_OUTPUTS value, "-" = unset> <hook mode> <use client 0/1> [config file]
case_run() {
  desc=$1
  outputs_val=$2
  hook=$3
  use_client=$4
  cfg=${5:-}
  log=$(mktemp)
  state_home=$(mktemp -d)
  envs=(GUIBUX_TEST_EXTRA_OUTPUTS=1 GUIBUX_TEST_OUTPUTS=$hook GUIBUX_TERM=true WLR_RENDERER=gles2 XDG_STATE_HOME=$state_home)
  if [ "$outputs_val" != "-" ]; then
    envs+=(GUIBUX_OUTPUTS=$outputs_val)
  fi
  if [ -n "$cfg" ]; then
    envs+=(GUIBUX_CONFIG=$cfg)
  fi
  env "${envs[@]}" "$COMP" >"$log" 2>&1 &
  comp=$!
  client=""
  if [ "$use_client" = 1 ]; then
    wd=""
    for i in $(seq 1 50); do
      wd=$(grep -oP 'WAYLAND_DISPLAY=\K\S+' "$log" | head -1)
      [ -n "$wd" ] && break
      sleep 0.1
    done
    if [ -z "$wd" ]; then
      echo "outputs-test: FAILED ($desc): no WAYLAND_DISPLAY"
      cat "$log"
      rm -f "$log" ${cfg:+"$cfg"}; rm -rf "$state_home"
      kill $comp 2>/dev/null
      fail=1
      return
    fi
    WAYLAND_DISPLAY=$wd "$CLIENT" &
    client=$!
  fi
  sleep 6
  [ -n "$client" ] && kill $client 2>/dev/null
  kill $comp 2>/dev/null
  wait $comp 2>/dev/null
  [ -n "$client" ] && wait $client 2>/dev/null
  if grep -q "outputs-test: OK" "$log" && ! grep -q "outputs-test: FAIL" "$log"; then
    echo "outputs-test: OK ($desc)"
    rm -f "$log" ${cfg:+"$cfg"}; rm -rf "$state_home"
  else
    echo "outputs-test: FAILED ($desc)"
    cat "$log"
    rm -f "$log" ${cfg:+"$cfg"}; rm -rf "$state_home"
    fail=1
  fi
}

case_run "bogus name ignored, auto-arrange" "NOPE-1@0x0" bogus 0
case_run "manual placement" "HEADLESS-1@0x0,HEADLESS-2@1280x0" placement 0
case_run "@off disables monitor" "HEADLESS-2@off" off 0
case_run "unplug rehomes windows" "-" unplug 1

# live re-apply: the compositor re-reads the config file it was started
# with (GUIBUX_CONFIG) and applies disable / re-enable / move without a
# restart
cfg=$(mktemp)
echo "outputs = HEADLESS-1@0x0,HEADLESS-2@1280x0" >"$cfg"
case_run "live re-apply (disable, re-enable, move)" "-" apply 0 "$cfg"

[ "$fail" -eq 0 ]
