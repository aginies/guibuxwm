# Project layout

```
meson.build      build definition (wlroots-0.20, freetype2, cairo deps)
build.sh         builds wlroots 0.20.2 if needed, then the WM
config/guibuxwm  sample config file (all defaults, copy to ~/.config/guibuxwm/config)
src/main.c       entry point, server setup
src/background.c desktop background (stb_image: PNG + JPEG)
src/stb_image.h  single-header image loader (stb, public domain)
src/output.c     output lifecycle, background/topbar creation
src/topbar.c     per-monitor topbar rendering
src/toplevel.c   xdg-shell toplevel handling
src/window-layout.c  tiling, workspaces, snap
src/launcher.c   command box
src/keyboard.c   key handling, keybinds
src/config.c     config file parsing
src/cursor.c     cursor motion, button, seat
src/popup.c      xdg-popup handling
src/sysinfo.c    network + battery sysinfo via D-Bus
src/wm-test.c    headless test helpers
tests/           headless test clients (ws-test, tile-test) + runner scripts (run-all.sh)
```

## src/main.c

Server initialization: Wayland display, global registry (xdg-shell, xdg-decoration),
seat, cursor, keyboard, pointer, output list, compositor creation, and startup hooks.

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
window pills for tiled windows, date/time, and configurable styling
(height, font size, colors, padding).

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
keybind table lookup and action dispatch.

## src/config.c

Config file parsing: `key = value` format, comments, type conversion,
validation, and error logging.

## src/cursor.c

Cursor motion, button handling, seat setup, and cursor image rendering.

## src/popup.c

xdg-popup handling: popup manager, surface creation, positioning, and
event routing.

## src/sysinfo.c

System information via D-Bus: network status, battery state, and
dynamic topbar updates.

## src/wm-test.c

Headless test helpers: virtual output creation, surface mapping,
event injection, and assertion macros used by the test clients.

## tests/

- `ws-test` — Wayland client that maps 2 toplevels and idles, used by
  the workspace state machine test
- `tile-test` — Wayland client that maps 3 toplevels and verifies
  tile mode layouts and re-packing
- `run-all.sh` — runs all tests
- `run-ws-test.sh` — workspace state machine test
- `run-tile-test.sh` — tiling test
- `run-topbar-test.sh` — topbar test
- `run-launcher-test.sh` — launcher test
- `run-config-test.sh` — config file test
