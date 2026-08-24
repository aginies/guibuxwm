#!/bin/bash
# Battery test: start the compositor with GUIBUX_TEST_BATTERY and let the
# ~6.5s hook verify the sysinfo UPower poll and the topbar battery
# indicator. The script probes upower for ground truth (battery present
# or not) and passes it via GUIBUX_TEST_BATTERY_EXPECT. No client needed.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
log=$(mktemp)
expect=""
if command -v upower >/dev/null 2>&1 && upower -d 2>/dev/null | grep -q "battery"; then
  expect="yes"
fi
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=0 GUIBUX_TEST_BATTERY=1 GUIBUX_TEST_BATTERY_EXPECT="$expect" GUIBUX_TERM=true WLR_RENDERER=gles2 "$COMP" >"$log" 2>&1 &
comp=$!
sleep 8
kill $comp 2>/dev/null
wait $comp 2>/dev/null
grep -E "battery-test" "$log"
if grep -q "battery-test: OK" "$log"; then
  rm -f "$log"
  exit 0
fi
echo "battery-test: FAILED"
cat "$log"
rm -f "$log"
exit 1
