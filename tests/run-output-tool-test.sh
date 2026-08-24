#!/bin/bash
# guibuxwm-output tool test: no compositor needed. A fake state file and a
# temp config file drive the tool; the SIGUSR1 path is verified against a
# dummy process (sleep), which dies on the signal.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
TOOL="$ROOT/build/guibuxwm-output"
fail=0
tmp=$(mktemp -d)
state_home="$tmp/state"
mkdir -p "$state_home/guibuxwm"
state="$state_home/guibuxwm/outputs"
cfg="$tmp/config"

dummy=""
cleanup() {
  [ -n "$dummy" ] && kill $dummy 2>/dev/null
  wait 2>/dev/null
  rm -rf "$tmp"
}
trap cleanup EXIT

fail_msg() {
  echo "output-tool-test: FAIL ($1)"
  fail=1
}

expect_line() {  # expect_line <desc> <expected outputs value>
  got=$(grep -oP '^outputs = \K.*' "$cfg")
  if [ "$got" != "$2" ]; then
    fail_msg "$1: config line '$got' != '$2'"
  fi
}

expect_grep() {  # expect_grep <desc> <pattern> <haystack>
  if ! echo "$3" | grep -q "$2"; then
    fail_msg "$1: no match for '$2'"
  fi
}

expect_no_grep() {  # expect_no_grep <desc> <pattern> <haystack>
  if echo "$3" | grep -q "$2"; then
    fail_msg "$1: unexpected match for '$2'"
  fi
}

run() {  # run the tool with the test env
  XDG_STATE_HOME="$state_home" GUIBUX_CONFIG="$cfg" "$TOOL" "$@"
}

# --- fake state: HEADLESS-1 at 1920x0 (modes incl. a 1920x10800 trap),
# --- HEADLESS-2 at 0x0 (single 1280x720 mode), pid 0 = not running
write_state() {  # write_state <pid>
  cat >"$state" <<EOF
# guibuxwm outputs: pid=$1
# name x y w h mode_w mode_h transform enabled modes
HEADLESS-1 1920 0 1280 720 1280 720 0 1 1920x10800@60,1280x720@60
HEADLESS-2 0 0 1280 720 1280 720 0 1 1280x720@60
EOF
}
write_state 0

# list: both outputs, positions, modes
out=$(run list 2>&1); rc=$?
[ $rc -eq 0 ] || fail_msg "list rc $rc != 0"
expect_grep "list names" "HEADLESS-1" "$out"
expect_grep "list position" "1920x0" "$out"
expect_grep "list modes" "1280x720@60" "$out"

# set: position + mode + transform
run set HEADLESS-2 1920 0 --mode 1280x720 --transform 90 --no-apply >/dev/null
expect_line "set" "HEADLESS-2@1920x0:1280x720:90"

# set with a mode not in the list: warning, still saved (transform kept)
out=$(run set HEADLESS-2 1920 0 --mode 640x480 --no-apply 2>&1)
expect_grep "mode warning" "warning" "$out"
expect_line "set bad mode saved" "HEADLESS-2@1920x0:640x480:90"

# set with a mode that is a substring trap (1920x1080 inside 1920x10800):
# must warn (no exact token)
out=$(run set HEADLESS-1 0 0 --mode 1920x1080 --no-apply 2>&1)
expect_grep "substring trap warns" "warning" "$out"

# set with an exact mode from the list: no warning
out=$(run set HEADLESS-1 0 0 --mode 1280x720 --no-apply 2>&1)
expect_no_grep "exact mode no warning" "warning" "$out"
expect_line "set exact mode" "HEADLESS-2@1920x0:640x480:90,HEADLESS-1@0x0:1280x720"

# disable: NAME@off
run disable HEADLESS-1 --no-apply >/dev/null
expect_line "disable" "HEADLESS-2@1920x0:640x480:90,HEADLESS-1@off"

# enable: x=y=0 entry takes the state position (1920x0), but that is taken
# by HEADLESS-2: auto-placed right of it (1920+1280) to extend, not mirror.
# The mode is gone: NAME@off does not encode a mode, so re-enabling falls
# back to the preferred mode
out=$(run enable HEADLESS-1 --no-apply 2>&1)
expect_grep "enable extends" "placing at 3200x0" "$out"
expect_line "enable" "HEADLESS-2@1920x0:640x480:90,HEADLESS-1@3200x0"

# brand-new enable on a config without an outputs line: appended, other
# lines kept, state position taken
cat >"$cfg" <<EOF
# my config
term = foot
EOF
run enable HEADLESS-1 --no-apply >/dev/null
expect_line "append enable" "HEADLESS-1@1920x0"
got=$(grep -c "^term = foot$" "$cfg")
[ "$got" -eq 1 ] || fail_msg "append keeps lines: term line missing"

# replace: old value gone, exactly one outputs line
run set HEADLESS-1 640 0 --no-apply >/dev/null
expect_line "replace" "HEADLESS-1@640x0"
n=$(grep -c "^outputs = " "$cfg")
[ "$n" -eq 1 ] || fail_msg "replace: $n outputs lines"

# set on a position already used by another enabled output: mirror warning,
# still saved (explicit coordinates are the user's call)
out=$(run set HEADLESS-2 640 0 --no-apply 2>&1)
expect_grep "set mirror warns" "mirrors, not extends" "$out"
expect_line "set mirror saved" "HEADLESS-1@640x0,HEADLESS-2@640x0"

# enable of a disabled output (state box 0x0) must not land on 0x0 under
# the primary: auto-placed right of it (0+1280)
cat >"$cfg" <<EOF
outputs = HEADLESS-1@0x0
EOF
cat >"$state" <<EOF
# guibuxwm outputs: pid=0
# name x y w h mode_w mode_h transform enabled modes
HEADLESS-1 0 0 1280 720 1280 720 0 1 1280x720@60
HEADLESS-2 0 0 0 0 0 0 0 0 1280x720@60
EOF
out=$(run enable HEADLESS-2 --no-apply 2>&1)
expect_grep "enable disabled extends" "placing at 1280x0" "$out"
expect_line "enable disabled extends" "HEADLESS-1@0x0,HEADLESS-2@1280x0"
write_state 0
cat >"$cfg" <<EOF
outputs = HEADLESS-1@0x0
EOF

# no state file: list fails, --no-apply still saves
rm -f "$state"
run list >/dev/null 2>&1; rc=$?
[ $rc -ne 0 ] || fail_msg "list without state: rc 0"
run set HEADLESS-1 0 0 --no-apply >/dev/null
expect_line "save without state" "HEADLESS-1@0x0"

# apply with pid=0: no signal, friendly message, rc 0
out=$(run apply 2>&1); rc=$?
[ $rc -eq 0 ] || fail_msg "apply no pid rc $rc != 0"
expect_grep "apply no pid message" "compositor not running" "$out"

# apply with a live pid: SIGUSR1 reaches the process (sleep dies on it)
sleep 300 &
dummy=$!
disown
write_state $dummy
run apply >/dev/null 2>&1; rc=$?
[ $rc -eq 0 ] || fail_msg "apply signals rc $rc != 0"
for i in $(seq 1 50); do
  kill -0 $dummy 2>/dev/null || break
  sleep 0.1
done
if kill -0 $dummy 2>/dev/null; then
  fail_msg "apply signals: dummy still alive"
fi
dummy=""

# --no-apply must not signal
sleep 300 &
dummy=$!
disown
write_state $dummy
run apply --no-apply >/dev/null 2>&1
sleep 0.5
if ! kill -0 $dummy 2>/dev/null; then
  fail_msg "--no-apply: dummy died"
fi
kill $dummy 2>/dev/null
dummy=""

# bad arguments
run set HEADLESS-2 0 >/dev/null 2>&1; rc=$?
[ $rc -ne 0 ] || fail_msg "set missing args: rc 0"
run frobnicate >/dev/null 2>&1; rc=$?
[ $rc -ne 0 ] || fail_msg "unknown command: rc 0"
run set HEADLESS-2 0 0 --mode 100 --no-apply >/dev/null 2>&1; rc=$?
[ $rc -ne 0 ] || fail_msg "bad --mode: rc 0"
run set HEADLESS-2 0 0 --transform 45 --no-apply >/dev/null 2>&1; rc=$?
[ $rc -ne 0 ] || fail_msg "bad --transform: rc 0"

if [ "$fail" -eq 0 ]; then
  echo "output-tool-test: OK"
  exit 0
fi
echo "output-tool-test: FAILED"
exit 1
