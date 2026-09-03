#!/bin/bash
# Alt+drag test: start the compositor with GUIBUX_TEST_ALTDRAG, map
# toplevels with the ws-test client, and let the ~2s hook verify that
# alt+left-drag moves a window and leaves it at the drop position.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
CLIENT="$ROOT/build/tests/ws-test"
log=$(mktemp)
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=0 GUIBUX_TEST_ALTDRAG=1 GUIBUX_TERM=true \
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
grep -E "altdrag-test" "$log"
if grep -q "altdrag-test: OK" "$log"; then
  rm -f "$log"
  exit 0
fi
echo "altdrag-test: FAILED"
cat "$log"
rm -f "$log"
exit 1
