#!/bin/bash
# Tooltip test: start the compositor with GUIBUX_TEST_TOOLTIP. The sysinfo
# worker seeds a fake battery (85%, discharging, 1h 30m left), the ~2s hook
# hovers the topbar battery indicator and verifies the tooltip shows with
# the right text, then hides on move-away. No client needed.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
log=$(mktemp)
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=0 GUIBUX_TEST_TOOLTIP=1 GUIBUX_TERM=true WLR_RENDERER=gles2 "$COMP" >"$log" 2>&1 &
comp=$!
sleep 4
kill $comp 2>/dev/null
wait $comp 2>/dev/null
grep -E "tooltip-test" "$log"
if grep -q "tooltip-test: OK" "$log"; then
  rm -f "$log"
  exit 0
fi
echo "tooltip-test: FAILED"
cat "$log"
rm -f "$log"
exit 1
