# Project layout

```
meson.build        build definition (wlroots-0.20, freetype2, cairo deps)
meson_options.txt  build options (effects)
build.sh           builds wlroots 0.20.2 if needed, then the WM
config/guibuxwm    sample config file (all defaults, copy to ~/.config/guibuxwm/config)
share/guibuxwm.desktop   session entry (installed to xsessions + wayland-sessions)
tools/guibuxwm-output.c  standalone monitor layout tool (edits config, SIGUSR1)
src/main.c         entry point, server setup, test hooks
src/guibuxwm.h     shared types, enums, constants, cross-file declarations
src/background.c   desktop background (stb_image: PNG + JPEG)
src/stb_image.h    single-header image loader (stb, public domain)
src/stb_image_write.h  single-header PNG writer (stb, public domain)
src/output.c       output lifecycle, background/topbar creation
src/topbar.c       per-monitor topbar rendering
src/topbar-items.c topbar items panel (Mod+l)
src/outputs-panel.c  monitor layout panel (Mod+m)
src/outputs-config.c/h  `outputs` spec parse/format (shared with the tool)
src/toplevel.c     xdg-shell toplevel handling
src/window-layout.c  tiling, workspaces, snap
src/window-restore.c remember/restore window positions (state file)
src/window-preview.c window preview on topbar pill hover
src/launcher.c     command box
src/switcher.c     Alt+Tab window switcher
src/keyboard.c     key handling, keybinds, volume, session env
src/config.c       config file parsing
src/cursor.c       cursor motion, button, seat
src/popup.c        xdg-popup handling
src/sysinfo.c      network + battery sysinfo via D-Bus, audio via pactl
src/overview.c     F12 GNOME-style overview (workspace grid, drag-to-move)
src/help.c         keybinding help overlay
src/screensaver.c  idle screensaver (DPMS off, inhibit-aware)
src/notify.c       desktop notifications (D-Bus daemon, panel, auto-hide)
src/effects.c      optional animations (window open/close, retile, panel slide)
src/gfx.c          shared drawing/buffer helpers (utf8, text width, shm buffer)
src/panel.c        shared overlay buffer/draw plumbing
src/osd.c          on-screen display (volume/mic/brightness)
src/power.c        power menu (suspend/hibernate/lock/logout/restart/shutdown)
src/lock.c         lock screen (PAM auth)
src/screenshot.c   screenshot (fullscreen/region/window)
src/tooltip.c      topbar indicator tooltips
tests/             headless test clients + runner scripts
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

## src/stb_image_write.h

Single-header PNG writer (stb, public domain), used by the screenshot
feature to encode frames on the worker thread.

## src/output.c

Output (monitor) lifecycle: probe, create/destroy, background and topbar creation,
virtual layout management (for `GUIBUX_OUTPUTS`), and output enumeration.

## src/topbar.c

Per-monitor topbar rendering: monitor letter (A, B, C...), workspace pills,
window pills for tiled windows, network/audio indicators, notification bell,
date/time, and configurable styling (height, font size, colors, padding).
Also the hit-test API the cursor code uses for pill/indicator clicks.

## src/topbar-items.c

Topbar items panel (`Mod+l`): enable/disable and reorder the right-side
indicators (network, volume, mic, battery, notifications, clock). Changes
apply immediately to the running topbars (runtime state; the config
`topbar_items` line is read at start and on reload).

## src/outputs-panel.c

Monitor layout panel (`Mod+m`): keyboard-driven editor for the `outputs`
config spec — list, reorder (left/right swap), mode, rotation, on/off.
Every change rewrites the `outputs` config line and re-applies it via
`outputs_apply`, the same flow the `guibuxwm-output` tool uses without
the signal round-trip. Connected outputs not in the config are seeded
from their live state so the panel manages the whole layout.

## src/outputs-config.c/h

The `outputs` spec format shared by the compositor and the
`guibuxwm-output` tool: `NAME@XxY[:WxH[:ROT]],NAME@off,...`. Parses and
formats the spec, reads/writes the `outputs` line of the config file,
and maps transform names to `WL_OUTPUT_TRANSFORM_*` values.

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

## src/window-restore.c

Window position restore (config `restore_positions`): remembers each app's
last monitor, workspace, position and size in a plain text state file at
`$XDG_STATE_HOME/guibuxwm/window-positions` (or
`~/.local/state/guibuxwm/window-positions`). One line per app:
`app_id|output|box_x|box_y|workspace|x|y|w|h`.

- saved when a window closes and on a clean compositor exit
  (`restore_save_all`, before the clients are destroyed)
- restored on map: the window goes back to its saved monitor/workspace
  (tile mode: placed by the tile layout instead)
- the monitor must exist at the same layout box (`box_x`/`box_y`): a
  same-name monitor at a different position is a replugged physical
  monitor and gets normal placement; so do missing monitors and
  off-screen positions
- the terminal app id is excluded; legacy 7-field lines (without the box)
  still load, with the monitor identity check disabled

## src/window-preview.c

Window preview: hovering a window pill in the topbar shows a scaled-down
capture of that window after a short delay. The capture is taken via the
renderer (texture readback) and drawn as a floating panel; it is removed
when the pointer leaves the pill or the window unmaps.

## src/launcher.c

Command box (`Mod+e`): shows an input field, matches `$PATH` commands,
supports preferred apps, handles Enter/Escape/mouse click. Includes icon
support: resolves icons from a configurable icon theme (or GTK icon theme),
loads PNG icons via stb_image, caches them per session, and draws them
to the left of app names. Also parses `.desktop` files and resolves their
`Icon=` entries through the same theme path.

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

## src/gfx.c

Shared drawing/buffer helpers used by every overlay (topbar, panels, OSD,
tooltips, lock screen, window borders, previews): utf8 decode/truncate,
freetype text width, cairo color/rounded-rect helpers, text drawing on
cairo surfaces, and XRGB8888 shm buffer creation.

## src/panel.c

Plumbing shared by every overlay the compositor draws itself (power menu,
topbar items, outputs panel, help, OSD, switcher, notification panel,
tooltips): buffer/scene-node lifecycle, the cairo draw session over an
XRGB8888 shm buffer, the translucent frame, and the centred-box hit test.
Panels draw in device pixels (logical size x output scale) and are
published at their logical size via the scene buffer's dest size, so they
stay sharp on fractional-scale outputs.

## src/osd.c

On-screen display for volume/mic/brightness changes and screenshots:
a centred box with label, value and a bar (or the saved path for
screenshots). Auto-hides after `osd_timeout_ms`; a new event restarts
the timer.

## src/power.c

Power menu (`Mod+p`): Suspend, Hibernate, Lock, Log out, Restart, Shut
down. Each action shells out to `loginctl` (or `systemd-inhibit` for
suspend/hibernate) on the user session.

## src/lock.c

Lock screen (`Mod+Shift+l`, also `lock_on_idle` on the screensaver
timeout): full-screen overlay with clock, date and password entry. Input
is consumed until the password is accepted. Authenticates via PAM
(`config/etc/pam.d/guibuxwm`); without PAM support the lock falls back
to `loginctl lock-session`. A failed attempt clears the password and
shows a status line.

## src/screenshot.c

Screenshot (`Mod+Print` monitor under the cursor, `Mod+Shift+Print`
drag a region, `Mod+Ctrl+Print` focused window). The GPU readback runs
on the main thread (the renderer is not thread-safe); the PNG encode and
disk write run on a worker thread to avoid compositor jank. Saves to
`$XDG_PICTURES_DIR` or `~/Pictures` as `guibuxwm-<timestamp>.png` and
shows the path in the OSD.

## src/tooltip.c

Tooltips for the topbar indicators (e.g. the battery time estimate):
a small panel that appears after a hover delay on an indicator and
follows the pointer while over it. Driven by `tooltip_tick` from the
topbar's 500 ms tick.

## src/guibuxwm.h

The shared header: all cross-file types (`struct guibux_server`,
`guibux_output`, `guibux_toplevel`, the panel/overlay state structs),
enums (actions, tile modes, cursor modes), constants (defaults, limits),
and the function declarations used across files. Internal helpers stay
`static` in their `.c` file.

## tests/

- `ws-test` — maps 2 toplevels, used by the workspace state machine test
- `tile-test` — maps 3 toplevels, verifies tile mode layouts and re-packing
- `effects-test` — verifies window close retile + open scale-in animations
- `psel-test` — primary selection test
- `restore-test` — maps a toplevel, used by the window position restore test
- `run-all.sh` — runs all tests
- `run-ws-test.sh` — workspace state machine test
- `run-tile-test.sh` — tiling test
- `run-topbar-test.sh` — topbar test
- `run-global-topbar-test.sh` — topbar on all monitors
- `run-audio-test.sh` — audio sysinfo poll + VOL/MIC indicators test
- `run-battery-test.sh` — battery sysinfo poll + BAT indicator test
- `run-launcher-test.sh` — launcher test
- `run-config-test.sh` — config file test
- `run-config-reload-test.sh` — live config reload (SIGHUP / keybind)
- `run-notify-test.sh` — notifications (D-Bus, auto-show, auto-hide, panel)
- `run-effects-test.sh` — window open/close animations
- `run-scroll-test.sh` — scroll over VOL/MIC indicators
- `run-altdrag-test.sh` — Alt+drag window move
- `run-xmondrag-test.sh` — cross-monitor drag
- `run-always-on-top-test.sh` — always-on-top (pin) windows
- `run-psel-test.sh` — primary selection
- `run-resize-test.sh` — window resize
- `run-overview-test.sh` — F12 overview
- `run-xwayland-test.sh` — XWayland support
- `run-restore-test.sh` — window position restore (save, restore, clean exit, replugged monitor, missing monitor)
- `run-outputs-test.sh` — `outputs` config placement
- `run-outputs-panel-test.sh` — outputs panel (Mod+m)
- `run-output-tool-test.sh` — guibuxwm-output tool (config edit + SIGUSR1)
- `run-topbar-items-test.sh` — topbar items panel (Mod+l)
- `run-lock-test.sh` — lock screen (overlay, PAM password)
- `run-screenshot-test.sh` — screenshot (fullscreen/region/window)
- `run-power-test.sh` — power menu (Mod+p)
- `run-osd-test.sh` — on-screen display (volume/brightness)
- `run-tooltip-test.sh` — topbar indicator tooltips
- `xdg-shell-protocol.c/h` — xdg-shell client protocol
- `primary-selection-client-protocol.c/h` — primary selection protocol
- `meson.build` — test client build rules
