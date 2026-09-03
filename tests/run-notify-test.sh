#!/bin/bash
# Notify test: start the compositor with GUIBUX_TEST_NOTIFY; the ~0.5s hook
# seeds a notification and verifies the topbar indicator renders on each
# output. Runs under a private session bus (dbus-run-session) so the
# D-Bus round-trip part (Notify in both spec signatures + replaces_id)
# always has a bus and owns the daemon name. No client needed.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
log=$(mktemp)
dbus-run-session -- bash -c '
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=0 GUIBUX_TEST_NET=testnet GUIBUX_TEST_TOOLTIP=1 GUIBUX_TEST_NOTIFY=1 GUIBUX_TERM=true WLR_RENDERER=vulkan "$0" >"$1" 2>&1 &
comp=$!
sleep 6
kill $comp 2>/dev/null
wait $comp 2>/dev/null
' "$COMP" "$log"
grep -E "notify-test" "$log"
if grep -q "notify-test: OK" "$log"; then
  rm -f "$log"
  exit 0
fi
echo "notify-test: FAILED"
cat "$log"
rm -f "$log"
exit 1
