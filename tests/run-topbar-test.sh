#!/bin/bash
# Topbar test: start the compositor with GUIBUX_TEST_TOPBAR and let the
# ~0.5s hook verify each output's topbar (number + time). No client needed.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
log=$(mktemp)
GUIBUX_OUTPUTS= GUIBUX_TEST_TOPBAR=1 GUIBUX_TERM=true WLR_RENDERER=gles2 "$COMP" >"$log" 2>&1 &
comp=$!
sleep 4
kill $comp 2>/dev/null
wait $comp 2>/dev/null
grep -E "topbar-test" "$log"
if grep -q "topbar-test: OK" "$log"; then
  rm -f "$log"
  exit 0
fi
echo "topbar-test: FAILED"
cat "$log"
rm -f "$log"
exit 1
