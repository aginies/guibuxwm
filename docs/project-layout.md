# Project layout

```
meson.build      build definition (wlroots-0.20, freetype2, cairo deps)
build.sh         builds wlroots 0.20.2 if needed, then the WM
config/guibuxwm  sample config file (all defaults, copy to ~/.config/guibuxwm/config)
src/main.c       entry point, server setup, test hooks
src/background.c desktop background (stb_image: PNG + JPEG)
src/stb_image.h  single-header image loader (stb, public domain)
src/output.c     output lifecycle, background/topbar creation
src/topbar.c     per-monitor topbar rendering
src/toplevel.c   xdg-shell toplevel handling
src/window-layout.c  tiling, workspaces, snap
src/launcher.c   command box
src/keyboard.c   key handling, keybinds, volume, session env
src/config.c     config file parsing
src/cursor.c     cursor motion, button, seat
src/popup.c      xdg-popup handling
src/sysinfo.c    network + battery sysinfo via D-Bus, audio via pactl
src/switcher.c   Alt+Tab window switcher
src/overview.c   F12 GNOME-style overview (workspace grid, drag-to-move)
src/help.c       keybinding help overlay
src/screensaver.c idle screensaver (DPMS off, inhibit-aware)
src/notify.c     desktop notifications (D-Bus daemon, panel, auto-hide)
src/effects.c    optional animations (window open/close, retile, panel slide)
tests/           headless test clients + runner scripts
```

## src/main.c

Server initialization: Wayland display, global registry (xdg-shell, xdg-decoration),
seat, cursor, keyboard, pointer, output list, compositor creation, XWayland,
notification daemon, screensaver, and startup hooks. Also sets up test timers
for all `GUIBUX_TEST_*` env vars.

## src/background.c

Desktop background rendering using stb_image (PNG + JPEG). Handles per-workspace
images, fallback to the default `background` image, and scale modes
(stretch, fit, fill, tile).

## src/stb_image.h

Single-header image loader (stb, public domain) for PNG and JPEG support.

## src/output.c

Output (monitor) lifecycle: probe, create/destroy, background and topbar creation,
virtual layout management (for `GUIBUX_OUTPUTS`), and output enumeration.

## src/topbar.c

Per-monitor topbar rendering: monitor letter (A, B, C...), workspace pills,
window pills for tiled windows, network/audio indicators, notification bell,
date/time, and configurable styling (height, font size, colors, padding).

## src/toplevel.c

xdg-shell toplevel handling: map/unmap, configure, activate, close.
Sets up XWayland `DISPLAY` for spawned clients and handles X11 toplevels.

## src/window-layout.c

Window layout management:

- **Cascading** (default): windows placed at offset positions based on z-order
- **Tile modes**: free, split, main+stack (per monitor)
- **Workspaces**: 4 workspaces per monitor, visibility tracking
- **Snap**: half-screen snap to left/right/top/bottom
- **Repack**: re-arrange windows after close/fullscreen changes

## src/launcher.c

Command box (`Mod+e`): shows an input field, matches `$PATH` commands,
supports preferred apps, handles Enter/Escape/mouse click.

## src/keyboard.c

Key handling and keybinds: keymap setup, xkb translation, modifier tracking,
keybind table lookup and action dispatch. Also handles volume changes via
`pactl`, session environment setup (systemd user manager), and SIGCHLD
reaping for spawned children.

## src/config.c

Config file parsing: `key = value` format, comments, type conversion,
validation, and error logging.

## src/cursor.c

Cursor motion, button handling, seat setup, and cursor image rendering.

## src/popup.c

xdg-popup handling: popup manager, surface creation, positioning, and
event routing.

## src/sysinfo.c

System information: network status and battery state via D-Bus
(NetworkManager, UPower), audio volume/mute via `pactl` (PulseAudio /
PipeWire), and dynamic topbar updates.

## src/switcher.c

Alt+Tab window switcher: lists all toplevels with `A1: title` labels,
highlights the selection, warps cursor to the selected window. Select
on Alt release or Enter. Escape to dismiss.

## src/overview.c

F12 GNOME-style overview: every output displays its 4 workspaces as rows
with window cells labeled `A1: title`. A semi-transparent dim covers each
output. A workspace column on the left edge shows all four workspaces
(`A1`..`A4`) for empty workspace visibility and drag-drop targets.

- drag a window onto a row (or its column cell) to move it to that
  workspace, including across monitors
- click an empty area to switch to the workspace of the clicked row
- click a window to select it (switches to its workspace)
- `1`..`4` switch to that workspace, `Esc`/`F12` close the overview

## src/help.c

Keybinding help overlay: renders all active keybinds (defaults + config)
in a centered box. Escape or Enter to dismiss.

## src/screensaver.c

Idle screensaver: turns off outputs (DPMS) after a configurable timeout.
Respects idle inhibitors and active UI overlays (launcher, switcher, help).
Re-arms on user activity.

## src/notify.c

Desktop notifications via `org.freedesktop.Notifications` (D-Bus daemon).
Implements `Notify`, `CloseNotification`, `GetCapabilities` (`body`) and
`GetServerInformation` (spec 1.3).

- **Topbar indicator:** bell + pending count
- **List panel:** right-aligned, up to 10 rows, "Clear all" button
- **Auto-show:** new notifications pop the panel
- **Auto-hide:** panel closes after 2s unless hovered
- **Graceful degradation:** works indicator-only if another daemon owns
  the D-Bus name

## src/effects.c

Optional animations (build option 'effects', config 'effects'):

- **Window open:** scale from 85% or slide from center
- **Window close:** snapshot shrinks to center point
- **Retile:** animated position + resize for remaining windows
- **Notification panel:** slide in/out from right edge

Uses ease-out cubic easing. Configurable duration via `effects_duration_ms`.

## tests/

- `ws-test` — maps 2 toplevels, used by the workspace state machine test
- `tile-test` — maps 3 toplevels, verifies tile mode layouts and re-packing
- `effects-test` — verifies window close retile + open scale-in animations
- `psel-test` — primary selection test
- `run-all.sh` — runs all tests
- `run-ws-test.sh` — workspace state machine test
- `run-tile-test.sh` — tiling test
- `run-topbar-test.sh` — topbar test
- `run-audio-test.sh` — audio sysinfo poll + VOL/MIC indicators test
- `run-launcher-test.sh` — launcher test
- `run-config-test.sh` — config file test
- `run-notify-test.sh` — notifications (D-Bus, auto-show, auto-hide, panel)
- `run-effects-test.sh` — window open/close animations
- `run-scroll-test.sh` — scroll over VOL/MIC indicators
- `run-altdrag-test.sh` — Alt+drag window move
- `run-psel-test.sh` — primary selection
- `run-resize-test.sh` — window resize
- `run-overview-test.sh` — F12 overview
- `run-xwayland-test.sh` — XWayland support
- `xdg-shell-protocol.c/h` — xdg-shell client protocol
- `primary-selection-client-protocol.c/h` — primary selection protocol
- `meson.build` — test client build rules
