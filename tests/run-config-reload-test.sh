#!/bin/bash
# Config reload test: start the compositor with a temp config, send SIGHUP
# after editing the file (new color + new keybind), and verify the reload
# log line and the re-registered keybind.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
log=$(mktemp)
cfg=$(mktemp)
cat >"$cfg" <<EOF
term = true
color_bg = #010203
topbar_bg = #040506
renderer = pixman
EOF
GUIBUX_OUTPUTS= GUIBUX_TEST_EXTRA_OUTPUTS=0 GUIBUX_TERM=true WLR_RENDERER=vulkan "$COMP" -c "$cfg" >"$log" 2>&1 &
comp=$!
sleep 3
# edit the config: new color + a new keybind
cat >"$cfg" <<EOF
term = true
color_bg = #aabbcc
topbar_bg = #040506
keybind = Mod+Shift+r: reload-config
renderer = pixman
EOF
kill -HUP $comp 2>/dev/null
sleep 2
kill $comp 2>/dev/null
wait $comp 2>/dev/null
ok=1
grep -q "config: SIGHUP, reloading config" "$log" || { echo "config-reload-test: FAIL SIGHUP not handled"; ok=0; }
grep -q "config: reloading" "$log" || { echo "config-reload-test: FAIL reload not logged"; ok=0; }
grep -q "config: color_bg = #aabbcc" "$log" || { echo "config-reload-test: FAIL new color not applied"; ok=0; }
grep -q "config: keybind 'Mod+Shift+r: reload-config'" "$log" || { echo "config-reload-test: FAIL new keybind not registered"; ok=0; }
grep -q "config: reload done" "$log" || { echo "config-reload-test: FAIL reload did not complete"; ok=0; }
grep -E "config: (SIGHUP|reloading|reload done|color_bg|keybind)" "$log"
if [ "$ok" -eq 1 ]; then
  rm -f "$log" "$cfg"
  exit 0
fi
echo "config-reload-test: FAILED"
cat "$log"
rm -f "$log" "$cfg"
exit 1
