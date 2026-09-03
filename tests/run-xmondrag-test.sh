#!/bin/bash
# Cross-monitor drag test: start the compositor with two headless outputs
# and GUIBUX_TEST_XMONDRAG, map toplevels with the ws-test client, and let
# the hook verify that dragging a window onto the other monitor (and back,
# via a resize) updates its stored output and both topbar window lists.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
CLIENT="$ROOT/build/tests/ws-test"
log=$(mktemp)
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=1 GUIBUX_TEST_XMONDRAG=1 GUIBUX_TERM=true \
  WLR_RENDERER=vulkan "$COMP" >"$log" 2>&1 &
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
grep -E "xmondrag-test" "$log"
if grep -q "xmondrag-test: OK" "$log"; then
  rm -f "$log"
  exit 0
fi
echo "xmondrag-test: FAILED"
cat "$log"
rm -f "$log"
exit 1
