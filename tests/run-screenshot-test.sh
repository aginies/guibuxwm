#!/bin/bash
# Screenshot test: start the compositor with GUIBUX_TEST_SCREENSHOT and let
# the ~2s hook capture the headless output to a PNG. No client needed.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
log=$(mktemp)
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=0 GUIBUX_TEST_SCREENSHOT=1 GUIBUX_TERM=true WLR_RENDERER=gles2 \
  "$COMP" >"$log" 2>&1 &
comp=$!
sleep 8
kill $comp 2>/dev/null
wait $comp 2>/dev/null
grep -E "screenshot-test" "$log"
if grep -q "screenshot-test: OK" "$log"; then
  rm -f "$log"
  exit 0
fi
echo "screenshot-test: FAILED"
cat "$log"
rm -f "$log"
exit 1
