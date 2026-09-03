#!/bin/bash
# Launcher (command box) test: start the compositor with
# GUIBUX_TEST_LAUNCHER_CMD and let the hook drive the box headlessly
# (show, type, Enter, show, Escape). No client needed.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
log=$(mktemp)
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=1 GUIBUX_TEST_LAUNCHER_CMD="echo ok > /tmp/guibux-launcher-test" \
  GUIBUX_TERM=true WLR_RENDERER=vulkan "$COMP" >"$log" 2>&1 &
comp=$!
sleep 4
kill $comp 2>/dev/null
wait $comp 2>/dev/null
grep -E "launcher-test" "$log"
if grep -q "launcher-test: ENTER OK" "$log" && grep -q "launcher-test: ESCAPE OK" "$log" \
  && ! grep -q "launcher-test: FAIL" "$log"; then
  rm -f "$log" /tmp/guibux-launcher-test
  exit 0
fi
echo "launcher-test: FAILED"
cat "$log"
rm -f "$log" /tmp/guibux-launcher-test
exit 1
