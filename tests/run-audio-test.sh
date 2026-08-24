#!/bin/bash
# Audio test: start the compositor with GUIBUX_TEST_AUDIO and let the
# ~6.5s hook verify the sysinfo audio poll (pactl) and the topbar
# VOL/MIC indicators. No client needed.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
log=$(mktemp)
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=0 GUIBUX_TEST_AUDIO=1 GUIBUX_TERM=true WLR_RENDERER=gles2 "$COMP" >"$log" 2>&1 &
comp=$!
sleep 8
kill $comp 2>/dev/null
wait $comp 2>/dev/null
grep -E "audio-test" "$log"
if grep -q "audio-test: OK" "$log"; then
  rm -f "$log"
  exit 0
fi
echo "audio-test: FAILED"
cat "$log"
rm -f "$log"
exit 1
