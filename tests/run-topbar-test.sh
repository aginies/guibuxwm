#!/bin/bash
# Topbar test: start the compositor with GUIBUX_TEST_TOPBAR and let the
# ~0.5s hook verify each output's topbar (number + time). No client needed.
# A second run disables some indicators via the topbar_items config key
# (GUIBUX_TEST_TOPBAR_DISABLED) and verifies they are not rendered.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
log=$(mktemp)
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=0 GUIBUX_TEST_TOPBAR=1 GUIBUX_TERM=true WLR_RENDERER=gles2 "$COMP" >"$log" 2>&1 &
comp=$!
sleep 4
kill $comp 2>/dev/null
wait $comp 2>/dev/null
grep -E "topbar-test" "$log"
if ! grep -q "topbar-test: OK" "$log"; then
  echo "topbar-test: FAILED"
  cat "$log"
  rm -f "$log"
  exit 1
fi
rm -f "$log"

# second run: disable network + volume + mic, keep battery + notifications + clock
cfg=$(mktemp)
cat >"$cfg" <<EOF
term = true
topbar_items = battery, notifications, clock
EOF
log=$(mktemp)
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=0 GUIBUX_TEST_TOPBAR=1 \
  GUIBUX_TEST_TOPBAR_DISABLED=network,volume,mic GUIBUX_TERM=true \
  WLR_RENDERER=gles2 "$COMP" -c "$cfg" >"$log" 2>&1 &
comp=$!
sleep 4
kill $comp 2>/dev/null
wait $comp 2>/dev/null
grep -E "topbar-test" "$log"
if grep -q "topbar-test: OK" "$log"; then
  rm -f "$log" "$cfg"
  exit 0
fi
echo "topbar-test: FAILED (disabled items run)"
cat "$log"
rm -f "$log" "$cfg"
exit 1
