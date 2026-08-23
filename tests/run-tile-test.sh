#!/bin/bash
# Tile mode test: start the compositor with GUIBUX_TEST_TILE_MODE, map 3
# toplevels with the tile-test client, and let it verify the configured
# sizes match the tile mode. The client prints the verdict and exit code.
# usage: run-tile-test.sh <tile-mode>   (0=free, 1=split, 2=main+stack)
set -u
mode=$1
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
CLIENT="$ROOT/build/tests/tile-test"
log=$(mktemp)
cfg=$(mktemp)
echo "term = true" >"$cfg"
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=0 GUIBUX_TEST_TILE_MODE=$mode GUIBUX_TERM=true \
  WLR_RENDERER=gles2 "$COMP" -c "$cfg" >"$log" 2>&1 &
comp=$!
wd=""
for i in $(seq 1 50); do
  wd=$(grep -oP 'WAYLAND_DISPLAY=\K\S+' "$log" | head -1)
  [ -n "$wd" ] && break
  sleep 0.1
done
if [ -z "$wd" ]; then echo "NO WAYLAND_DISPLAY"; kill $comp; cat "$log"; rm -f "$log"; exit 2; fi
sleep 0.5
WAYLAND_DISPLAY=$wd "$CLIENT" "$mode"
rc=$?
kill $comp 2>/dev/null
wait $comp 2>/dev/null
rm -f "$log" "$cfg"
exit $rc
