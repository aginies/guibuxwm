# Testing

Tests live in `tests/` and run headless (no real display). Each is a
compositor-side hook (a `GUIBUX_TEST_*` env var) driven by a runner script;
some also use a small Wayland client (built by the main build) that maps
real toplevels. The compositor log / client output is the verdict.

## Running tests

### Run everything

```sh
tests/run-all.sh
```

### Run individually

```sh
tests/run-ws-test.sh [ws]        # workspace state machine (default ws 2)
tests/run-tile-test.sh <mode>    # tiling: 0=free, 1=split, 2=main+stack
tests/run-topbar-test.sh         # per-output topbar (number + time)
tests/run-audio-test.sh          # audio sysinfo poll + VOL/MIC indicators
tests/run-battery-test.sh        # battery sysinfo poll (UPower) + topbar indicator
tests/run-tooltip-test.sh        # topbar hover tooltips (battery time estimate, net IP/GW/DNS)
tests/run-launcher-test.sh       # command box (show/type/enter/escape, preferred apps)
tests/run-config-test.sh         # config file (term, keybind, colors)
tests/run-outputs-test.sh        # monitor arrangement (auto, manual, @off, unplug, live re-apply)
tests/run-output-tool-test.sh    # guibuxwm-output tool (config editing, state file, SIGUSR1)
tests/run-notify-test.sh         # notifications (D-Bus round-trip, auto-show, auto-hide, panel)
tests/run-effects-test.sh        # window close retile + open scale-in animations
tests/run-scroll-test.sh         # scroll over VOL/MIC indicators
tests/run-altdrag-test.sh        # Alt+drag window move
tests/run-xmondrag-test.sh       # drag/resize a window across monitors (output + topbar lists)
tests/run-always-on-top-test.sh  # always-on-top pin stays above a focused non-pinned window
tests/run-psel-test.sh           # primary selection
tests/run-resize-test.sh         # window resize
tests/run-overview-test.sh       # F12 overview
tests/run-xwayland-test.sh       # XWayland support
tests/run-restore-test.sh        # window position restore (save, restore, clean exit, replugged monitor)
```

## Headless mode

The compositor can also be run headless directly for smoke testing:

```sh
WLR_BACKENDS=headless WLR_RENDERER=gles2 ./build/guibuxwm
```

## GUIBUX_TEST_EXTRA_OUTPUTS

`GUIBUX_TEST_EXTRA_OUTPUTS=N` forces the headless backend and creates
N+1 virtual 1280x720 monitors (the headless backend has no default output),
useful to exercise the multi-monitor code paths:

```sh
GUIBUX_TEST_EXTRA_OUTPUTS=1 WLR_RENDERER=gles2 ./build/guibuxwm
```

## GUIBUX_TEST_LAUNCHER_CMD

`GUIBUX_TEST_LAUNCHER_CMD="command"` drives the command box headlessly:
shortly after start it shows the box, types the command, presses Enter
(then shows and Escapes again), logging `launcher-test: ENTER OK` /
`ESCAPE OK` on success. If the config defines preferred apps, it also
verifies that Up selects the one closest to the input line
(`launcher-test: PREFERRED UP OK`). The icon probe logs the resolved icon
path for the selected match (`launcher-test: ICON '...'`) or `(none)` if
the theme has no such icon:

```sh
GUIBUX_TEST_EXTRA_OUTPUTS=1 GUIBUX_TEST_LAUNCHER_CMD="echo ok > /tmp/x" \
  WLR_RENDERER=gles2 ./build/guibuxwm
```

## GUIBUX_TEST_TILE_MODE

`GUIBUX_TEST_TILE_MODE=N` sets the tile mode of all outputs shortly after
start (0=free, 1=split, 2=main+stack), to exercise the tiling code paths:

```sh
GUIBUX_TEST_EXTRA_OUTPUTS=0 GUIBUX_TEST_TILE_MODE=1 GUIBUX_TERM=true \
  WLR_RENDERER=gles2 ./build/guibuxwm
```

The `tile-test` client (in `tests/`, built by the main build) maps 3
toplevels, verifies the configured sizes for the given mode, closes one
window and verifies the re-pack, then fullscreens a window and verifies it
returns to its tile slot.

## GUIBUX_TEST_LOCK

`GUIBUX_TEST_LOCK=1` verifies the lock screen ~2s after start: the overlay
is shown on every output with a valid buffer, typing appends and backspace
removes (including multi-byte UTF-8 as a unit), Esc does not unlock, and
hiding clears the password. If `GUIBUX_TEST_LOCK_PASSWORD` is set (and PAM
is available at build time), it also verifies that a wrong password is
rejected (`fail_count` increments, still locked) and the correct password
unlocks. Logs `lock-test: OK` on success. No client needed:

```sh
GUIBUX_OUTPUTS= GUIBUX_TEST_LOCK=1 GUIBUX_TERM=true WLR_RENDERER=gles2 \
  ./build/guibuxwm
# with PAM auth verification:
GUIBUX_TEST_LOCK_PASSWORD="s3cret" GUIBUX_OUTPUTS= GUIBUX_TEST_LOCK=1 \
  GUIBUX_TERM=true WLR_RENDERER=gles2 ./build/guibuxwm
```

## GUIBUX_TEST_TOPBAR

`GUIBUX_TEST_TOPBAR=1` verifies the topbars shortly after start (one bar per
output, monitor letter matches the left-to-right layout order (A = leftmost),
non-empty date/time string), logging `topbar-test: OK (N outputs)` on success:

```sh
GUIBUX_TEST_EXTRA_OUTPUTS=1 GUIBUX_TEST_TOPBAR=1 GUIBUX_TERM=true \
  WLR_RENDERER=gles2 ./build/guibuxwm
```

## GUIBUX_TEST_AUDIO

`GUIBUX_TEST_AUDIO=1` verifies the audio sysinfo poll (~6.5s after start,
after the first `pactl` poll at ~5s): when an audio system is available the
volumes must be in 0..100 and the topbar VOL/MIC indicators must be
rendered; without audio the indicators must be absent. Logs
`audio-test: OK (N outputs, audio on|off)` on success:

```sh
GUIBUX_OUTPUTS= GUIBUX_TEST_AUDIO=1 GUIBUX_TERM=true \
  WLR_RENDERER=gles2 ./build/guibuxwm
```

## GUIBUX_TEST_BATTERY

`GUIBUX_TEST_BATTERY=1` verifies the battery sysinfo poll (~6.5s after
start, after the first UPower poll at ~5s). The runner script probes
`upower -d` for ground truth and passes it via
`GUIBUX_TEST_BATTERY_EXPECT=yes|""`: with a battery device the polled
value must be a `NN%` string and the topbar indicator must be rendered;
without one the value must stay empty. When `upower -d` shows a
"time to empty/full" line the runner also passes
`GUIBUX_TEST_BATTERY_ETA=yes` and the polled time estimate must be
non-zero (the topbar tooltip shows the remaining time). Logs
`battery-test: OK (N outputs, battery NN%|off)` on success:

```sh
GUIBUX_OUTPUTS= GUIBUX_TEST_BATTERY=1 GUIBUX_TERM=true \
  WLR_RENDERER=gles2 ./build/guibuxwm
```

## GUIBUX_TEST_TOOLTIP

`GUIBUX_TEST_TOOLTIP=1` (together with `GUIBUX_TEST_NET=<label>`)
verifies the topbar hover tooltips ~2s after start. The sysinfo worker
seeds a fake battery (85%, discharging, 1h 30m left) and one fake net
iface (IP `10.0.0.5`, DNS `1.1.1.1`, GW `10.0.0.1`) so the test runs
without UPower or NetworkManager. The hook hovers the battery indicator
and the first net segment in turn, verifying:

- the battery tooltip shows the percentage and time estimate
- the net tooltip is multi-line (label + IP + GW + DNS) and the polled
  per-device details match the seeded values
- each tooltip hides when the pointer moves away

Logs `tooltip-test: OK (battery + net)` on success. No client needed:

```sh
GUIBUX_OUTPUTS= GUIBUX_TEST_TOOLTIP=1 GUIBUX_TEST_NET=eth0 GUIBUX_TERM=true \
  WLR_RENDERER=gles2 ./build/guibuxwm
```

## GUIBUX_TEST_OSD

`GUIBUX_TEST_OSD=1` verifies the on-screen display ~2s after start: shows
a fake volume OSD (65%), checks the box is on-screen and the state is
stored, then forces the timeout and checks the OSD hides. Logs
`osd-test: OK (volume 65%, box WxH at X,Y)` on success. No client needed:

```sh
GUIBUX_OUTPUTS= GUIBUX_TEST_OSD=1 GUIBUX_TERM=true \
  WLR_RENDERER=gles2 ./build/guibuxwm
```

## GUIBUX_TEST_POWER

`GUIBUX_TEST_POWER=1` verifies the power menu ~2s after start: shows the
panel, checks the box height, hit-tests a row (must resolve to Suspend),
checks an out-of-bounds point misses, and checks Esc closes the panel.
Logs `power-test: OK (6 actions, box WxH)` on success. No client needed:

```sh
GUIBUX_OUTPUTS= GUIBUX_TEST_POWER=1 GUIBUX_TERM=true \
  WLR_RENDERER=gles2 ./build/guibuxwm
```

## GUIBUX_TEST_WORKSPACES

`GUIBUX_TEST_WORKSPACES=N` (default 2) exercises the workspace state machine
~2s after start: all outputs start on workspace 1 with every window visible,
then every output switches to N (windows hide, keyboard focus clears), a
window is moved to N and back, and switching back restores visibility. Logs
`workspace-test: OK (N outputs, ws M, K toplevels)` on success.

The `ws-test` client (in `tests/`, built by the main build) maps 2 toplevels
and idles, giving the test real windows:

```sh
GUIBUX_TEST_EXTRA_OUTPUTS=1 GUIBUX_TEST_WORKSPACES=2 GUIBUX_TERM=true \
  WLR_RENDERER=gles2 ./build/guibuxwm &
# in another shell:
./build/tests/ws-test
```

## GUIBUX_TEST_KEYBIND

`GUIBUX_TEST_KEYBIND="key"` sends `Mod+key` through the keybind table
shortly after start and expects the launcher to open (used by the config
test to verify a custom keybind):

```sh
GUIBUX_TEST_EXTRA_OUTPUTS=1 GUIBUX_TEST_KEYBIND=g \
  GUIBUX_CONFIG=/path/to/config WLR_RENDERER=gles2 ./build/guibuxwm
```

## GUIBUX_TEST_NOTIFY

`GUIBUX_TEST_NOTIFY=1` exercises the notification stack. It runs under a
private session bus (`dbus-run-session`) so the compositor owns the
`org.freedesktop.Notifications` name. A ~0.5s hook seeds notifications and,
on later ticks, verifies:

- the D-Bus round-trip in both spec signatures (`Notify` with and without
  `replaces_id`) parses the strings and the replace keeps the same id
- a new notification **auto-shows** the panel, which **auto-hides** after the
  2s delay (checked on later ticks, with a no-check window around the timer
  to avoid a race)
- the topbar bell indicator renders on every output
- the panel hit areas (rows, "Clear all") match the visible position and the
  rows render text pixels
- clicking the bell opens the panel

Logs `notify-test: OK (N outputs, K notifications)` on success. No client
needed.

```sh
tests/run-notify-test.sh
```

## GUIBUX_TEST_QUIT

`GUIBUX_TEST_QUIT=<ms>` terminates the compositor after `<ms>` (default
2000) — the same clean exit as the quit keybind. Used by the restore test
to verify that still-mapped windows are saved on a clean exit:

```sh
GUIBUX_TEST_QUIT=3000 WLR_RENDERER=gles2 ./build/guibuxwm
```

## Test clients

Test clients are built by the main build and live in `tests/`:

- `ws-test` — maps 2 toplevels, used by the workspace state machine test
- `tile-test` — maps 3 toplevels, verifies tile mode layouts and re-packing
- `restore-test` — maps a toplevel with a fixed app id; `keep` mode stays
  mapped so the compositor exits first (clean-exit save)

## Summary of env vars

| Variable | Purpose |
|---|---|
| `GUIBUX_TEST_EXTRA_OUTPUTS=N` | Force N+1 virtual 1280x720 outputs |
| `GUIBUX_TEST_LAUNCHER_CMD="cmd"` | Type `cmd` in the launcher headlessly; also probes icon resolution for the selected match |
| `GUIBUX_TEST_TILE_MODE=N` | Set tile mode (0=free, 1=split, 2=main+stack) |
| `GUIBUX_TEST_LOCK=1` | Verify lock screen (overlay, typing, PAM auth if password set) |
| `GUIBUX_TEST_LOCK_PASSWORD="pw"` | PAM auth verification for the lock test |
| `GUIBUX_TEST_TOPBAR=1` | Verify topbars after start |
| `GUIBUX_TEST_AUDIO=1` | Verify audio poll + VOL/MIC indicators |
| `GUIBUX_TEST_BATTERY=1` | Verify battery poll (UPower) + topbar indicator |
| `GUIBUX_TEST_WORKSPACES=N` | Exercise workspace state machine (default 2) |
| `GUIBUX_TEST_KEYBIND="key"` | Verify a custom keybind opens the launcher |
| `GUIBUX_TEST_NOTIFY=1` | Verify notifications (D-Bus, auto-show, auto-hide, panel) |
| `GUIBUX_TEST_QUIT=<ms>` | Terminate the compositor after `<ms>` (clean exit, same as the quit keybind) |
| `GUIBUX_TEST_SCROLL=1` | Scroll over VOL/MIC indicators in the topbar |
| `GUIBUX_TEST_ALTDRAG=1` | Alt+drag window move |
| `GUIBUX_TEST_XMONDRAG=1` | Drag/resize a window across monitors: stored output + topbar lists must follow |
| `GUIBUX_TEST_ALWAYS_ON_TOP=1` | Pin one window, focus another: the pinned window must stay on top |
| `GUIBUX_TEST_PRIMARY_SELECTION=1` | Primary selection (middle-click paste) |
| `GUIBUX_TEST_RESIZE=1` | Window resize |
| `GUIBUX_TEST_OVERVIEW=1` | F12 overview |
| `GUIBUX_TERM=true` | Spawn a terminal client for tests |
| `WLR_BACKENDS=headless` | Force headless backend |
| `WLR_RENDERER=vulkan` | Use Vulkan renderer (tests run on Vulkan) |
