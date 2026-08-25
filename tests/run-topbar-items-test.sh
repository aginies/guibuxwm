#!/bin/bash
# Topbar items panel test: start the compositor with GUIBUX_TEST_TOPBAR_ITEMS
# and let the ~2s hook verify the panel (shown, correct height, row
# hit-testing, d toggles, Esc closes). No client needed.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
log=$(mktemp)
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=0 GUIBUX_TEST_TOPBAR_ITEMS=1 GUIBUX_TERM=true WLR_RENDERER=gles2 "$COMP" >"$log" 2>&1 &
comp=$!
sleep 4
kill $comp 2>/dev/null
wait $comp 2>/dev/null
grep -E "topbar-items-test" "$log"
if grep -q "topbar-items-test: OK" "$log"; then
  rm -f "$log"
  exit 0
fi
echo "topbar-items-test: FAILED"
cat "$log"
rm -f "$log"
exit 1
