# Features

## Window Management

- **xdg-shell windows:** focus, move, resize, fullscreen
- **X11 via XWayland:** X11 windows (e.g. flatpak apps) map as regular toplevels with focus, tiling, fullscreen, workspaces and topbar entries; `DISPLAY` is set automatically for spawned clients, `Mod+drag` moves an X11 window (it has no titlebar)
- **Focus follows mouse** (keyboard focus tracks cursor, configurable)
- **Cascading window placement**

## Overview

- **GNOME-style overview** (`F12`): all workspaces and windows visible, click to select, drag a window onto another workspace row (or another monitor) to move it

## Tiling

- **Tile modes** (`Mod+t`): free / left-right split / main+stack, per monitor
- **Snap to half-screen** (`Mod+Left` / `Mod+Right`): left or right 50% of monitor

## Command Box

- **Command box** (`Mod+e`) with icon support: launch programs by typing, with `$PATH` suggestions, up to 5 configurable preferred apps (each with optional icons), and `.desktop` file icon resolution through the configured icon theme

## Topbar

- **Topbar** on each monitor: monitor letter (A, B, C, ...) on the left, date and time on the right (updates every second); tiled windows laid out below it; network status and audio indicators (scroll to adjust volume, right-click to open `pavucontrol` or `nmtui`)

## Workspaces

- **Workspaces** per monitor (4, numbered 1–4): `Mod+1..4` switch, `Mod+Shift+1..4` move the focused window; workspace numbers shown in the topbar with the current one highlighted (clickable to switch)

## Configuration

- **Terminal** command configurable (default: `gnome-terminal`), started by `Mod+Return`
- **Keyboard layout** configurable (xkb layout, variant, options)
- **Config file** for keybinds, terminal, keyboard, colors, backgrounds, icons, effects, and more

## Desktop Background

- **Desktop background** image (PNG or JPEG) with per-workspace images and configurable scale mode (stretch / fit / fill / tile)

## Multi-monitor

- **Multi-monitor support:** new windows open under the mouse cursor, move windows between monitors, optional manual monitor arrangement (position, per-monitor mode, rotation, disable) via the `outputs` config key or `GUIBUX_OUTPUTS`
- **Live monitor layout:** the `guibuxwm-output` tool places, re-modes, rotates, enables and disables monitors of a running compositor without a restart

## Window Position Restore

- **Window position restore:** each app's last monitor, workspace, position and size are remembered and restored on the next launch (state file under `XDG_STATE_HOME`, terminal excluded)

## Session Environment

- **Session environment:** sets `XDG_SESSION_TYPE` / `XDG_CURRENT_DESKTOP` for spawned apps, imports `DISPLAY` / `WAYLAND_DISPLAY` into the systemd user manager (restarting xdg-desktop-portal), so clicking a URL in an app opens the default browser

## Notifications

- **Desktop notifications:** registers as the session notification daemon (`org.freedesktop.Notifications`), shows a topbar bell indicator with count, and a right-aligned panel with auto-show/auto-hide

## Animations

- **Optional animations** for window open/close, retile, and notification panel
