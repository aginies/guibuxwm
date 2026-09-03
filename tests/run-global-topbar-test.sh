#!/bin/bash
# Global topbar test: start the compositor with two headless outputs and the
# GUIBUX_TEST_GLOBAL_TOPBAR hook, map 2 toplevels with the ws-test client,
# and let the ~2s hook verify both bars list both windows (own-monitor
# windows first). Compositor log is the verdict.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
CLIENT="$ROOT/build/tests/ws-test"
log=$(mktemp)
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=1 GUIBUX_TEST_GLOBAL_TOPBAR=1 GUIBUX_TERM=true \
  WLR_RENDERER=vulkan "$COMP" >"$log" 2>&1 &
comp=$!
sleep 2
WAYLAND_DISPLAY=$(grep -o 'WAYLAND_DISPLAY=[^ ]*' "$log" | head -1 | cut -d= -f2)
if [ -z "$WAYLAND_DISPLAY" ]; then
  echo "global-topbar-test: FAIL no WAYLAND_DISPLAY in log"
  cat "$log"
  kill $comp 2>/dev/null
  rm -f "$log"
  exit 1
fi
GUIBUX_TERM=true WLR_RENDERER=vulkan WAYLAND_DISPLAY="$WAYLAND_DISPLAY" "$CLIENT" >/dev/null 2>&1 &
client=$!
sleep 4
kill $client 2>/dev/null
wait $client 2>/dev/null
kill $comp 2>/dev/null
wait $comp 2>/dev/null
grep -E "global-topbar-test" "$log"
if grep -q "global-topbar-test: OK" "$log"; then
  rm -f "$log"
  exit 0
fi
echo "global-topbar-test: FAILED"
cat "$log"
rm -f "$log"
exit 1
