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
tests/run-launcher-test.sh       # command box (show/type/enter/escape, preferred apps)
tests/run-config-test.sh         # config file (term, keybind, colors)
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
(`launcher-test: PREFERRED UP OK`):

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

## Test clients

Test clients are built by the main build and live in `tests/`:

- `ws-test` — maps 2 toplevels, used by the workspace state machine test
- `tile-test` — maps 3 toplevels, verifies tile mode layouts and re-packing

## Summary of env vars

| Variable | Purpose |
|---|---|
| `GUIBUX_TEST_EXTRA_OUTPUTS=N` | Force N+1 virtual 1280x720 outputs |
| `GUIBUX_TEST_LAUNCHER_CMD="cmd"` | Type `cmd` in the launcher headlessly |
| `GUIBUX_TEST_TILE_MODE=N` | Set tile mode (0=free, 1=split, 2=main+stack) |
| `GUIBUX_TEST_TOPBAR=1` | Verify topbars after start |
| `GUIBUX_TEST_AUDIO=1` | Verify audio poll + VOL/MIC indicators |
| `GUIBUX_TEST_WORKSPACES=N` | Exercise workspace state machine (default 2) |
| `GUIBUX_TEST_KEYBIND="key"` | Verify a custom keybind opens the launcher |
| `GUIBUX_TERM=true` | Spawn a terminal client for tests |
| `WLR_BACKENDS=headless` | Force headless backend |
| `WLR_RENDERER=gles2` | Use GLES2 renderer |
