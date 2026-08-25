#!/bin/bash
# XWayland test: start the compositor headless, launch an X11 client
# (xterm) against the Xwayland display, and verify the compositor maps
# and later destroys the xwayland toplevel.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
log=$(mktemp)
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=1 GUIBUX_TEST_XWAYLAND=1 GUIBUX_TERM=true \
  WLR_RENDERER=gles2 "$COMP" >"$log" 2>&1 &
comp=$!
disp=""
for i in $(seq 1 50); do
  disp=$(grep -oP 'xwayland: DISPLAY=\K\S+' "$log" | head -1)
  [ -n "$disp" ] && break
  sleep 0.1
done
if [ -z "$disp" ]; then echo "NO XWAYLAND DISPLAY"; kill $comp; cat "$log"; rm -f "$log"; exit 2; fi
DISPLAY=$disp xterm -T xwayland-test &
xt=$!
mapped=""
for i in $(seq 1 150); do
  if grep -q "mapped xwayland toplevel 'xwayland-test'" "$log"; then
    mapped=1
    break
  fi
  sleep 0.1
done
if [ -z "$mapped" ]; then
  echo "xwayland-test: FAIL no mapped toplevel"
  kill $xt $comp 2>/dev/null
  cat "$log"
  rm -f "$log"
  exit 1
fi
kill $xt 2>/dev/null
destroyed=""
for i in $(seq 1 50); do
  if grep -q "destroyed xwayland toplevel" "$log"; then
    destroyed=1
    break
  fi
  sleep 0.1
done
if [ -n "$destroyed" ]; then
  kill $comp 2>/dev/null
  wait $comp 2>/dev/null
  rm -f "$log"
  exit 0
fi
echo "xwayland-test: FAIL no destroyed toplevel"
kill $comp 2>/dev/null
cat "$log"
rm -f "$log"
exit 1
