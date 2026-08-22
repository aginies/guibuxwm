#!/bin/bash
# Workspace test: start the compositor with GUIBUX_TEST_WORKSPACES, map 2
# toplevels with the ws-test client, and let the compositor's ~2s hook
# verify the workspace state machine. The compositor log is the verdict.
# usage: run-ws-test.sh [ws]   (default 2)
set -u
ws=${1:-2}
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
CLIENT="$ROOT/build/tests/ws-test"
log=$(mktemp)
GUIBUX_TEST_EXTRA_OUTPUTS=1 GUIBUX_TEST_WORKSPACES=$ws GUIBUX_TERM=true \
  WLR_RENDERER=gles2 "$COMP" >"$log" 2>&1 &
comp=$!
wd=""
for i in $(seq 1 50); do
  wd=$(grep -oP 'WAYLAND_DISPLAY=\K\S+' "$log" | head -1)
  [ -n "$wd" ] && break
  sleep 0.1
done
if [ -z "$wd" ]; then echo "NO WAYLAND_DISPLAY"; kill $comp; cat "$log"; rm -f "$log"; exit 2; fi
WAYLAND_DISPLAY=$wd "$CLIENT" &
client=$!
sleep 6
kill $client $comp 2>/dev/null
wait $client 2>/dev/null
wait $comp 2>/dev/null
grep -E "workspace-test|workspace:" "$log"
if grep -q "workspace-test: OK" "$log"; then
  rm -f "$log"
  exit 0
fi
echo "workspace-test: FAILED"
cat "$log"
rm -f "$log"
exit 1
