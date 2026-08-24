#!/bin/bash
# Effects test: start the compositor with GUIBUX_TEST_EFFECTS, run the
# effects-test client (maps 2 windows, destroys one on the compositor's
# close request, maps a third late), and let the compositor's hook
# verify the close retile and the open scale-in.
# The compositor log is the verdict.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
CLIENT="$ROOT/build/tests/effects-test"
log=$(mktemp)
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=1 GUIBUX_TEST_EFFECTS=1 GUIBUX_TERM=true \
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
sleep 8
kill $client $comp 2>/dev/null
wait $client 2>/dev/null
wait $comp 2>/dev/null
grep -E "effects-test" "$log"
if grep -q "effects-test: OK" "$log"; then
  rm -f "$log"
  exit 0
fi
echo "effects-test: FAILED"
cat "$log"
rm -f "$log"
exit 1
