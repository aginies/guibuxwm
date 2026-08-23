#!/bin/bash
# Primary selection test: start the compositor with
# GUIBUX_TEST_PRIMARY_SELECTION, map 1 toplevel with the psel-test client,
# and let the compositor's ~1.5s hook deliver a pointer enter so the client
# can set the primary selection and read it back. The client output is the
# verdict.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
CLIENT="$ROOT/build/tests/psel-test"
log=$(mktemp)
out=$(mktemp)
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=1 GUIBUX_TEST_PRIMARY_SELECTION=1 GUIBUX_TERM=true \
  WLR_RENDERER=gles2 "$COMP" >"$log" 2>&1 &
comp=$!
wd=""
for i in $(seq 1 50); do
  wd=$(grep -oP 'WAYLAND_DISPLAY=\K\S+' "$log" | head -1)
  [ -n "$wd" ] && break
  sleep 0.1
done
if [ -z "$wd" ]; then echo "NO WAYLAND_DISPLAY"; kill $comp; cat "$log"; rm -f "$log" "$out"; exit 2; fi
WAYLAND_DISPLAY=$wd "$CLIENT" >"$out" 2>&1 &
client=$!
sleep 8
kill $client $comp 2>/dev/null
wait $client 2>/dev/null
wait $comp 2>/dev/null
cat "$out"
if grep -q "psel-test: OK" "$out"; then
  rm -f "$log" "$out"
  exit 0
fi
echo "psel-test: FAILED"
cat "$log"
rm -f "$log" "$out"
exit 1
