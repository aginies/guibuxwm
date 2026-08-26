#!/bin/bash
# Lock screen test: start the compositor with GUIBUX_TEST_LOCK and let the
# ~2s hook verify the lock (shown, buffers, typing, backspace, Esc does not
# unlock, PAM auth if GUIBUX_TEST_LOCK_PASSWORD is set, hide clears the
# password). No client needed.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
log=$(mktemp)
if [ -n "${GUIBUX_TEST_LOCK_PASSWORD:-}" ]; then
  export GUIBUX_TEST_LOCK_PASSWORD
fi
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=0 GUIBUX_TEST_LOCK=1 GUIBUX_TERM=true WLR_RENDERER=gles2 \
  "$COMP" >"$log" 2>&1 &
comp=$!
sleep 8
kill $comp 2>/dev/null
wait $comp 2>/dev/null
grep -E "lock-test" "$log"
if grep -q "lock-test: OK" "$log"; then
  rm -f "$log"
  exit 0
fi
echo "lock-test: FAILED"
cat "$log"
rm -f "$log"
exit 1
