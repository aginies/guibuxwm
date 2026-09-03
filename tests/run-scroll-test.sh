#!/bin/bash
# Scroll test: start the compositor with GUIBUX_TEST_SCROLL and let the
# ~6.5s hook scroll over the topbar VOL indicator, verifying that a
# scroll-up raises the published volume by one step. The system volume
# is restored by the test itself. No client needed.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
log=$(mktemp)
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=0 GUIBUX_TEST_SCROLL=1 GUIBUX_TERM=true WLR_RENDERER=vulkan "$COMP" >"$log" 2>&1 &
comp=$!
sleep 8
kill $comp 2>/dev/null
wait $comp 2>/dev/null
grep -E "scroll-test" "$log"
if grep -qE "scroll-test: (OK|SKIP)" "$log"; then
  rm -f "$log"
  exit 0
fi
echo "scroll-test: FAILED"
cat "$log"
rm -f "$log"
exit 1
