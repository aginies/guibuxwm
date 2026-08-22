# guibuxwm

A simple Wayland window manager built on [wlroots 0.20](https://gitlab.freedesktop.org/wlroots/wlroots).
Derived from [tinywl](https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/main/tinywl) (MIT).

## Features

- xdg-shell application windows: focus, move, resize, fullscreen
- Tile modes (`Mod+t`): free / left-right split / main+stack, per monitor
- Command box (`Mod+e`) to launch programs by typing a command, with
  `$PATH` command suggestions (Up/Down to select)
- Topbar on each monitor: monitor letter (A, B, C, ...) on the left, date
  and time on the right (updates every second); tiled windows are laid out
  below it
- Workspaces per monitor (4, numbered 1 2 3 4): `Mod+1..4` switch,
  `Mod+Shift+1..4` move the focused window; workspace numbers shown in the
  topbar with the current one highlighted (clickable to switch)
- Starts a terminal automatically at launch (default: `gnome-terminal`)
- Configurable keyboard layout (e.g. French), variant and options
- Config file for keybinds, terminal, keyboard and colors
- Multi-monitor support:
  - new windows open on the output under the mouse cursor
  - move windows between monitors with a keybind
  - optional manual monitor arrangement
- Cascading window placement

## Requirements

Tested on openSUSE. All development packages are expected to be preinstalled:

- `gcc` (or `clang`), `meson`, `ninja`
- `wayland-devel`, `wayland-protocols`
- `libdrm-devel`, `libinput-devel`, `libxkbcommon-devel`
- `freetype-devel`, `cairo-devel`
- `pixman-devel`, `libegl-devel` / `libgles-devel` (Mesa)
- `libudev-devel`, `libdisplay-info-devel`, `lcms2-devel`, `libliftoff-devel`, `libseat-devel`
- a Wayland terminal for the startup terminal (e.g. `gnome-terminal`)

wlroots itself is **not** taken from the distribution package: `build.sh`
builds wlroots 0.20.2 from source (see [Build](#build)).

## Build

```sh
./build.sh
```

`./build.sh clean` removes the `build/` directory (including the binary)
without touching the wlroots installation.

The script:

1. checks for wlroots 0.20.2 via `pkg-config` (module `wlroots-0.20`)
2. if missing: downloads the [wlroots 0.20.2 tarball](https://gitlab.freedesktop.org/wlroots/wlroots/-/archive/0.20.2/wlroots-0.20.2.tar.gz),
   builds it and installs it to `~/.local` (override with `WLR_PREFIX`)
3. runs `meson setup build` + `ninja -C build`

Manual build (if wlroots is already installed):

```sh
export PKG_CONFIG_PATH=$HOME/.local/lib64/pkgconfig:$PKG_CONFIG_PATH
meson setup build
ninja -C build
```

Run:

```sh
./build/guibuxwm
```

## Keybindings

`Mod` is the Super key. All bindings can be changed via the config file
(see [Keybinds](#keybinds)).

| Shortcut | Action |
|---|---|
| `Mod+Return` | Start a new terminal |
| `Mod+e` | Command box: type a command, Enter runs it via `sh -c` |
| `Mod+q` | Close focused window |
| `Mod+f` | Toggle fullscreen |
| `Mod+t` | Cycle tile mode of the focused window's monitor (free / split / main+stack) |
| `Mod+Tab` | Cycle window focus |
| `Mod+1..4` | Switch to workspace 1..4 on the focused window's monitor |
| `Mod+Shift+1..4` | Move focused window to workspace 1..4 (same monitor) |
| `Mod+Shift+Left` / `Mod+Shift+Right` | Move focused window to previous/next monitor |
| `Mod+Shift+q` | Quit |
| `Alt+Escape` | Quit |

Mouse: click a window to focus it, drag its titlebar to move, drag its
edges to resize. Dragging or resizing a fullscreen window leaves fullscreen.

### Command box (`Mod+e`)

Opens an input box centered on the monitor under the cursor. Type a command
and press Enter to run it via `/bin/sh -c` (so pipes, redirections and
environment variables work). Escape or a mouse click dismisses it without
running anything.

As you type, matching commands from `$PATH` are listed below the input line
(up to 8, exact matches first). Navigate with Up/Down; Enter runs the
selected command, with any arguments you typed after the first word appended
(e.g. type `alac -w`, select `alacritty`, Enter runs `alacritty -w`). With
no match, Enter runs exactly what you typed.

### Tile modes (`Mod+t`)

`Mod+t` cycles the tile mode of the monitor holding the focused window.
Each monitor keeps its own mode.

| Mode | Layout |
|---|---|
| free | default: cascading placement, free move/resize |
| split | windows fill two 50% columns round-robin, stacked vertically within a column |
| main+stack | focused window takes the left 50%, remaining windows stack vertically in the right 50% |

In tile modes, new windows are placed into the layout (instead of cascading),
closing a window repacks the remaining ones, and leaving fullscreen returns
the window to its slot. Dragging or resizing a window takes it out of the
layout until the next retiling (`Mod+t`, window map/close, fullscreen off,
or moving a window between monitors).

## Configuration

Configuration is via a config file, command-line flags and environment
variables. Priority: command-line flag > config file > `GUIBUX_*` env >
standard env > default.

### Config file

Location: `~/.config/guibuxwm/config` (override with the `-c` flag or the
`GUIBUX_CONFIG` env var). A missing file is skipped silently; malformed lines
are logged and ignored. Format: one `key = value` per line, `#` comments.

A sample with all defaults is in [`config/guibuxwm`](config/guibuxwm) — copy
it to `~/.config/guibuxwm/config` and edit.

| Key | Meaning | Example |
|---|---|---|
| `term` | Terminal command started at launch (and by `Mod+Return`) | `term = foot` |
| `xkb_layout` | Keyboard layout (xkb layout name) | `xkb_layout = fr` |
| `xkb_variant` | Keyboard variant | `xkb_variant = osd` |
| `xkb_options` | xkb options (comma-separated) | `xkb_options = caps:swapscape` |
| `keybind` | Keybinding, repeatable, see [Keybinds](#keybinds) | `keybind = Mod+g: launcher` |
| `color_bg` | Topbar/launcher background, `#rrggbb` | `color_bg = #1e1e2e` |
| `color_border` | Topbar bottom border / launcher border | `color_border = #45475a` |
| `color_highlight` | Highlight (launcher selection, current workspace cell) | `color_highlight = #3a3c55` |
| `color_text` | Text and cursor | `color_text = #ffffff` |
| `color_dim` | Dimmed text (launcher non-selected, inactive workspaces) | `color_dim = #8888aa` |
| `topbar_bg` | Topbar background (default: openSUSE green) | `topbar_bg = #73ba25` |
| `topbar_text` | Topbar text (monitor letter, workspaces, date/time) | `topbar_text = #1e1e2e` |

Example:

```
# ~/.config/guibuxwm/config
term = foot
xkb_layout = fr
keybind = Mod+g: launcher
keybind = Mod+Shift+q: quit
color_bg = #1e1e2e
```

### Keybinds

Syntax: `keybind = MODS+key: action`. At least one modifier is required:
`Mod` (or `Super`), `Shift`, `Alt`, `Ctrl`. `key` is an xkb key name
(`q`, `Return`, `Tab`, `F1`, ...). Actions:

| Action | Meaning |
|---|---|
| `terminal` | Start a new terminal |
| `close` | Close the focused window |
| `fullscreen` | Toggle fullscreen of the focused window |
| `tile` | Cycle tile mode of the focused window's monitor |
| `launcher` | Open the command box |
| `focus-next` | Cycle window focus |
| `quit` | Quit the compositor |
| `workspace:N` | Switch to workspace N (1..4) |
| `move-workspace:N` | Move the focused window to workspace N (1..4) |
| `move-monitor-left` / `move-monitor-right` | Move the focused window to the previous/next monitor |

A config keybind with the same modifiers+key as a default replaces it,
otherwise it is added. Defaults are listed in [Keybindings](#keybindings).

### Command-line flags

```
guibuxwm [-t terminal command] [-k keyboard layout] [-c config file]
```

| Flag | Meaning | Example |
|---|---|---|
| `-t` | Terminal command started at launch (and by `Mod+Return`) | `-t "gnome-terminal"` |
| `-k` | Keyboard layout (xkb layout name) | `-k fr` |
| `-c` | Path to the config file | `-c ~/.config/guibuxwm/config` |

### Environment variables

| Variable | Meaning | Example |
|---|---|---|
| `GUIBUX_CONFIG` | Config file path (overridden by `-c`) | `GUIBUX_CONFIG=~/wm.conf` |
| `GUIBUX_TERM` | Terminal command (overridden by `-t` and config `term`) | `GUIBUX_TERM="foot"` |
| `GUIBUX_XKB_LAYOUT` | Keyboard layout (overridden by `-k` and config `xkb_layout`) | `GUIBUX_XKB_LAYOUT="fr"` |
| `XKB_DEFAULT_LAYOUT` | Keyboard layout, standard xkb env (lowest priority) | `XKB_DEFAULT_LAYOUT="fr,us"` |
| `GUIBUX_OUTPUTS` | Manual monitor arrangement, see below | see below |
| `GUIBUX_TEST_EXTRA_OUTPUTS` | Test-only, see [Testing](#testing) | `GUIBUX_TEST_EXTRA_OUTPUTS=1` |

Terminal default: `gnome-terminal`. Keyboard layout default: system default
(usually `us`).

### Multi-monitor arrangement (`GUIBUX_OUTPUTS`)

By default monitors are arranged automatically (left to right, in the order
they appear). To place them manually:

```
GUIBUX_OUTPUTS="NAME@XxY,NAME@XxY,..."
```

 - `NAME` is the output name reported by wlroots (e.g. `DP-1`, `HDMI-A-1`,
   `eDP-1`). Find yours in the compositor log or with `wlr-randr`/`weston-info`.
 - `XxY` is the top-left position of the monitor in the virtual layout.
 - `:ROT` (optional) sets the rotation: `normal`, `90`, `180` or `270`
   (degrees clockwise). Example: `HDMI-A-1@0x0:90`.

Example — two monitors side by side, `HDMI-A-1` to the right of `DP-1`:

```sh
GUIBUX_OUTPUTS="DP-1@0x0,HDMI-A-1@1920x0" ./build/guibuxwm
```

Example — stacked vertically:

```sh
GUIBUX_OUTPUTS="DP-1@0x0,HDMI-A-1@0x1080" ./build/guibuxwm
```

Outputs not listed are still auto-arranged. Malformed entries are logged and
ignored. Up to 8 outputs can be placed manually.

Note: `GUIBUX_OUTPUTS` controls the position and rotation of monitors in the
virtual layout — the video mode always comes from the monitor's preferred
mode.

### Example based on a real session

A laptop with an external portrait monitor on the left and a landscape
monitor on the right (as reported by `xrandr --query | grep connected`):

```
HDMI-1 connected 1440x2560+0+0 right (normal left inverted right x axis y axis)
DP-8 connected primary 2560x1440+1440+414 (normal left inverted right x axis y axis)
```

reproduces with (wlroots uses the full DRM connector names, so `HDMI-1`
becomes `HDMI-A-1`; xrandr's `right` rotation is 90 degrees clockwise):

```sh
GUIBUX_OUTPUTS="HDMI-A-1@0x0:90,DP-8@1440x414" ./build/guibuxwm
```

To find your own layout, run the compositor once and read the output names
from the log, or use `xrandr --query | grep connected` (X11/XWayland) or
`wlr-randr` (under a wlroots compositor).

### Multi-monitor behavior

- **New windows** open on the monitor under the mouse cursor (cascaded).
- **`Mod+Shift+Left` / `Mod+Shift+Right`** moves the focused window to the
  previous/next monitor (monitors ordered left to right by layout position),
  centered on the target monitor.
- **Fullscreen** fills the monitor the window is on. A client that requests
  fullscreen on a specific monitor (e.g. some terminals) is honored.

## Testing

Tests live in `tests/` and run headless (no real display). Each is a
compositor-side hook (a `GUIBUX_TEST_*` env var) driven by a runner script;
some also use a small Wayland client (built by the main build) that maps
real toplevels. The compositor log / client output is the verdict.

Run everything:

```sh
tests/run-all.sh
```

Or individually:

```sh
tests/run-ws-test.sh [ws]        # workspace state machine (default ws 2)
tests/run-tile-test.sh <mode>    # tiling: 0=free, 1=split, 2=main+stack
tests/run-topbar-test.sh         # per-output topbar (number + time)
tests/run-launcher-test.sh       # command box (show/type/enter/escape)
tests/run-config-test.sh         # config file (term, keybind, colors)
```

The compositor can also be run headless directly for smoke testing:

```sh
WLR_BACKENDS=headless WLR_RENDERER=gles2 ./build/guibuxwm
```

`GUIBUX_TEST_EXTRA_OUTPUTS=N` forces the headless backend and creates
N+1 virtual 1280x720 monitors (the headless backend has no default output),
useful to exercise the multi-monitor code paths:

```sh
GUIBUX_TEST_EXTRA_OUTPUTS=1 WLR_RENDERER=gles2 ./build/guibuxwm
```

`GUIBUX_TEST_LAUNCHER_CMD="command"` drives the command box headlessly:
shortly after start it shows the box, types the command, presses Enter
(then shows and Escapes again), logging `launcher-test: ENTER OK` /
`ESCAPE OK` on success:

```sh
GUIBUX_TEST_EXTRA_OUTPUTS=1 GUIBUX_TEST_LAUNCHER_CMD="echo ok > /tmp/x" \
  WLR_RENDERER=gles2 ./build/guibuxwm
```

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

`GUIBUX_TEST_TOPBAR=1` verifies the topbars shortly after start (one bar per
output, monitor letter matches the left-to-right layout order (A = leftmost),
non-empty date/time string), logging `topbar-test: OK (N outputs)` on success:

```sh
GUIBUX_TEST_EXTRA_OUTPUTS=1 GUIBUX_TEST_TOPBAR=1 GUIBUX_TERM=true \
  WLR_RENDERER=gles2 ./build/guibuxwm
```

`GUIBUX_TEST_WORKSPACES=N` (default 2) exercises the workspace state machine
~2s after start: all outputs start on workspace 1 with every window visible,
then every output switches to N (windows hide, keyboard focus clears), a
window is moved to N and back, and switching back restores visibility. Logs
`workspace-test: OK (N outputs, ws M, K toplevels)` on
success. The `ws-test` client (in `tests/`, built by the main build) maps
2 toplevels and idles, giving the test real windows:

```sh
GUIBUX_TEST_EXTRA_OUTPUTS=1 GUIBUX_TEST_WORKSPACES=2 GUIBUX_TERM=true \
  WLR_RENDERER=gles2 ./build/guibuxwm &
# in another shell:
./build/tests/ws-test
```

`GUIBUX_TEST_KEYBIND="key"` sends `Mod+key` through the keybind table
shortly after start and expects the launcher to open (used by the config
test to verify a custom keybind):

```sh
GUIBUX_TEST_EXTRA_OUTPUTS=1 GUIBUX_TEST_KEYBIND=g \
  GUIBUX_CONFIG=/path/to/config WLR_RENDERER=gles2 ./build/guibuxwm
```

## Project layout

```
meson.build      build definition (wlroots-0.20, freetype2, cairo deps)
build.sh         builds wlroots 0.20.2 if needed, then the WM
config/guibuxwm  sample config file (all defaults, copy to ~/.config/guibuxwm/config)
src/main.c       the whole compositor (~2500 lines, incl. command box, topbar, workspaces and tiling)
tests/           headless test clients (ws-test, tile-test) + runner scripts (run-all.sh)
```

## License

MIT. Derived from tinywl, part of wlroots (MIT).
