#!/bin/bash
# Config file test: start the compositor with a temp config file (term,
# custom keybind, colors) and let the GUIBUX_TEST_KEYBIND hook fire the
# custom keybind headlessly. Marker file + compositor log are the verdict.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
COMP="$ROOT/build/guibuxwm"
log=$(mktemp)
cfg=$(mktemp)
marker=$(mktemp -u)
cat >"$cfg" <<EOF
term = touch $marker
keybind = Mod+g: launcher
color_bg = #010203
topbar_bg = #040506
EOF
GUIBUX_TEST_EXTRA_OUTPUTS=1 GUIBUX_TEST_KEYBIND=g \
  WLR_RENDERER=gles2 "$COMP" -c "$cfg" >"$log" 2>&1 &
comp=$!
sleep 4
kill $comp 2>/dev/null
wait $comp 2>/dev/null
ok=1
[ -f "$marker" ] || { echo "config-test: FAIL term not spawned"; ok=0; }
grep -q "config: keybind 'Mod+g: launcher'" "$log" || { echo "config-test: FAIL keybind not registered"; ok=0; }
grep -q "config: color_bg = #010203" "$log" || { echo "config-test: FAIL color not parsed"; ok=0; }
grep -q "config: topbar_bg = #040506" "$log" || { echo "config-test: FAIL topbar color not parsed"; ok=0; }
grep -q "keybind-test: OK" "$log" || { echo "config-test: FAIL keybind not dispatched"; ok=0; }
grep -E "config:|keybind-test" "$log"
if [ "$ok" -eq 1 ]; then
  rm -f "$log" "$cfg" "$marker"
  exit 0
fi
echo "config-test: FAILED"
cat "$log"
rm -f "$log" "$cfg" "$marker"
exit 1
