# Features

## Window Management

- **xdg-shell windows:** focus, close, move, resize, fullscreen (fullscreen renders below the topbar, so the bar stays visible)
- **X11 via XWayland:** X11 windows (e.g. flatpak apps) map as regular toplevels with focus, tiling, fullscreen, workspaces and topbar entries; `DISPLAY` is set automatically for spawned clients, `Mod+drag` moves an X11 window (it has no titlebar)
- **Interactive move/resize:** drag a window's titlebar to move it, drag its edges to resize it; dragging or resizing a fullscreen window leaves fullscreen
- **`Alt`+left-drag** moves any window (GNOME-style); the window stays where it is dropped. Works on Wayland and X11 windows
- **Focus follows mouse** (keyboard focus tracks cursor, configurable)
- **Cascading window placement** in free mode
- **Window switcher** (`Alt+Tab`): on-screen list of window titles (up to 16 lines) on the monitor under the cursor; `Tab` cycles, releasing `Alt` or `Enter` selects, `Esc` dismisses; the cursor warps to the selected window
- **Primary selection** (`wl_primary_selection_v1`) for X11-style mouse selection

## Overview

- **GNOME-style overview** (`F12`): every monitor shows its 4 workspaces as rows with the windows of each workspace as labeled cells (`A1: title`), plus a workspace column on the left edge so empty workspaces stay visible; a semi-transparent dim covers each output
- drag a window onto a workspace row (or its column cell) to move it, including across monitors
- click an empty area to switch to the workspace of the clicked row, click a window to select it, `1`..`4` switch workspaces, `Esc`/`F12` close

## Tiling

- **Tile modes** (`Mod+t`): free / left-right split / main+stack, per monitor, persisted per workspace
- in tile modes new windows are placed into the layout, closing a window repacks the remaining ones, and leaving fullscreen returns the window to its slot
- **Snap to half-screen** (`Mod+Left` / `Mod+Right` / `Mod+Ctrl+Shift+Up` / `Mod+Ctrl+Shift+Down`): left, right, top or bottom 50% of the monitor

## Command Box

- **Command box** (`Mod+e`) with icon support: launch programs by typing, with `$PATH` suggestions, up to 5 configurable preferred apps (each with optional icons), and `.desktop` file icon resolution through the configured icon theme

## Topbar

- **Topbar** on each monitor: monitor letter (A, B, C, ...) on the left, window pills, workspace numbers (click to switch), network status, audio and battery indicators, notification bell, and date/time on the right (updates every second); tiled windows are laid out below it
- **Window pills:** one per window on the current workspace; click to focus (switches to the window's workspace first if needed), double-click to toggle fullscreen
- **Network indicator:** SSID for WiFi, interface name for Ethernet, or "No net"/"NM" when unavailable, updated via NetworkManager D-Bus; right-click opens `nmtui`
- **Audio indicators** (`VOL` / `MIC`, PulseAudio or PipeWire via `pactl`): scroll to adjust by 1% per step, left-click toggles mute, right-click opens `pavucontrol`
- **Battery indicator** (UPower): `BAT NN%` with a hover tooltip showing the time estimate

## Workspaces

- **Workspaces** per monitor (4, numbered 1–4): `Mod+1..4` switch, `Mod+Shift+1..4` move the focused window, `Ctrl+Alt+Left`/`Right` switch to the previous/next workspace
- **Per-workspace backgrounds** and **per-workspace tile mode**: each workspace of each monitor keeps its own background image and tile mode
- workspace numbers shown in the topbar with the current one highlighted (clickable to switch)

## Configuration

- **Config file** (`~/.config/guibuxwm/config` or `-c`): keybinds, terminal, keyboard layout, colors, backgrounds, icon theme, topbar, effects, monitor layout, position restore, and more
- **Terminal** command configurable (default: `gnome-terminal`), started by `Mod+Return`
- **Keyboard layout** configurable (xkb layout, variant, options)
- **Keybinding help overlay** (`Mod+h`): lists all active keybinds on the monitor under the cursor

## Desktop Background

- **Desktop background** image (PNG or JPEG) with per-workspace images and configurable scale mode (stretch / fit / fill / tile)

## Multi-monitor

- **Multi-monitor support:** new windows open under the mouse cursor, move windows between monitors (`Mod+Shift+Left`/`Right` or by dragging), windows crossing a monitor border are reassigned to it
- **Manual monitor arrangement** via the `outputs` config key or `GUIBUX_OUTPUTS`: position, per-monitor mode, rotation, disable
- **Live monitor layout:** the `guibuxwm-output` tool places, re-modes, rotates, enables and disables monitors of a running compositor without a restart; `SIGUSR1` or the `outputs-apply` keybind re-applies the config
- **Outputs panel** (`Mod+m`): interactive monitor layout editing on screen — list, reorder (left/right swap), mode, rotation, on/off — every change is saved to the `outputs` config line and applied live

## Window Position Restore

- **Window position restore:** each app's last monitor, workspace, position and size are remembered and restored on the next launch (state file under `XDG_STATE_HOME`); the configured terminal is excluded (`term_app_id`), a missing or moved monitor falls back to cascading placement

## Session Environment

- **Session environment:** sets `XDG_SESSION_TYPE` / `XDG_CURRENT_DESKTOP` for spawned apps, imports `DISPLAY` / `WAYLAND_DISPLAY` into the systemd user manager (restarting xdg-desktop-portal), so clicking a URL in an app opens the default browser

## Notifications

- **Desktop notifications:** registers as the session notification daemon (`org.freedesktop.Notifications`), shows a topbar bell indicator with count, and a right-aligned panel with auto-show/auto-hide; clicking a row focuses the sending window

## Animations

- **Optional animations** for window open/close, retile, and the notification panel (configurable duration and style)

## Screensaver

- **Idle screensaver:** turns the monitors off (DPMS) after a configurable idle timeout, wakes on input; apps can keep the screen on via the `wl_idle_inhibit_v1` protocol (e.g. video playback), and `wl_idle_notify_v1` is exposed for idle observers

## Media Keys and System Controls

- **Hardware media keys** work without any keybind: volume up/down, mute, mic mute, brightness up/down
- **Keybind actions** for volume up/down, mute, mic volume up/down, mic mute, and brightness up/down (via `pactl` and `brightnessctl`)
