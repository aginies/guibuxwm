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
- **xdg-activation-v1:** background apps (portals, `gtk-launch`, scripts) can request focus of a window they spawned; the compositor switches to the window's workspace, focuses it and warps the cursor to it

## Overview

- **GNOME-style overview** (`F12`): every monitor shows its 4 workspaces as rows with the windows of each workspace as labeled cells (`A1: title`), plus a workspace column on the left edge so empty workspaces stay visible; a semi-transparent dim covers each output
- drag a window onto a workspace row (or its column cell) to move it, including across monitors
- click an empty area to switch to the workspace of the clicked row, click a window to select it, `1`..`4` switch workspaces, `Esc`/`F12` close

## Tiling

- **Tile modes** (`Mod+t`): free / left-right split / main+stack, per monitor, persisted per workspace
- in tile modes new windows are placed into the layout, closing a window repacks the remaining ones, and leaving fullscreen returns the window to its slot
- **Snap to half-screen** (`Mod+Left` / `Mod+Right` / `Mod+Ctrl+Shift+Up` / `Mod+Ctrl+Shift+Down`): left, right, top or bottom 50% of the monitor

## Command Box

- **Command box** (`Mod+e`) with icon support: launch programs by typing, with `$PATH` suggestions, up to 6 configurable preferred apps (each with optional icons), and `.desktop` file icon resolution through the configured icon theme
- **Command box triggers:** also opens by moving the mouse into the top-left corner of any monitor (hot corner, 8px) or by left-clicking the monitor letter badge (A, B, ...) on the topbar

## Topbar

- **Topbar** on each monitor: monitor letter (A, B, C, ...) on the left (left-click opens the command box), workspace mini-map cells (click to switch; the active cell is highlighted, no numbers), window pill list (click to focus, drag to move to a workspace), network status, audio and battery indicators, notification bell, and date/time on the right (updates every second); tiled windows are laid out below it
- **Topbar styling:** a faint 1px highlight line along the top edge (raised look against the wallpaper) and a 1px `color_border` line along the bottom edge; groups (workspaces / mini-map / window pills / indicator items / clock) are divided by 3px vertical separators with a vertical alpha gradient (fading out at both ends) so they read as soft dividers; every item keeps a clear margin from its separators
- **Window mini-map:** each workspace cell shows one small rect per own-monitor window on that workspace, positioned proportionally to the window's real on-screen position (left/right and over/below); fullscreen fills the whole cell; the focused window is highlighted (decorative — the clickable pills are in the window list); cells are divided by faint 1px vertical separators
- **Window pills:** a global list — this monitor's windows first, then the windows of the other monitors after a separator (the `A2:` prefix, monitor letter + workspace number, disambiguates); click to focus (switches to the window's monitor and workspace first if needed), double-click to toggle fullscreen
- **Drag window to workspace:** press a window pill and drag to a workspace cell to move the window to that workspace; the cell under the cursor is highlighted while dragging; a short click (no drag) still focuses/switches as before
- **Window preview:** hovering a window pill for half a second shows a live thumbnail of the window (480x300) below the pill, so the content of a window on another workspace is visible without switching to it; the preview tracks the window's live buffer and hides when the pointer leaves
- **Network indicator:** SSID for WiFi, interface name for Ethernet, or "No net"/"NM" when unavailable, updated via NetworkManager D-Bus; right-click opens `nmtui`; hovering an interface shows a multi-line tooltip with its IP, gateway and DNS servers
- **Audio indicators** (`VOL` / `MIC`, PulseAudio or PipeWire via `pactl`): scroll to adjust by 1% per step, left-click toggles mute, right-click opens `pavucontrol`; each shows a speaker/mic icon from the icon theme next to the text
- **Battery indicator** (UPower): `BAT NN%` with a hover tooltip showing the time estimate; the text turns red at 20% or below and orange at 50% or below (discharging only — charging stays the normal color); a battery icon reflects the charge level
- **Network indicator**: per-interface labels with a network icon from the icon theme
- **Configurable topbar items** (`topbar_items`): the right-side indicators (network, volume, mic, battery, notifications, clock) can be enabled, disabled or reordered per config; live-reloadable via SIGHUP; `topbar_items_output` restricts indicators + clock to a single monitor (default `all` = every monitor)
- **Topbar gradient** (`topbar_gradient` + `topbar_bg2`): optional left-to-right gradient on the topbar background, from `topbar_bg` to `topbar_bg2`; live-reloadable via SIGHUP
- **Topbar items panel** (`Mod+l`): a popup listing all six indicators with an on/off column; `d` or Enter toggles the selected row, changes apply live to every topbar immediately

## Workspaces

- **Workspaces** per monitor (4, numbered 1–4): `Mod+1..4` switch, `Mod+Shift+1..4` move the focused window, `Ctrl+Alt+Left`/`Right` switch to the previous/next workspace
- **Per-workspace backgrounds** and **per-workspace tile mode**: each workspace of each monitor keeps its own background image and tile mode
- workspace numbers shown in the topbar with the current one highlighted (clickable to switch)

## Configuration

- **Config file** (`~/.config/guibuxwm/config` or `-c`): keybinds, terminal, keyboard layout, colors, backgrounds, icon theme, topbar, effects, monitor layout, position restore, and more
- **Config reload:** `SIGHUP` or the `reload-config` keybind action re-reads the config file live (colors, keybinds, topbar, backgrounds, outputs, screensaver, icon theme, preferred apps); `renderer`, `xkb_*` and `touchpad_tap` require a restart
- **Touchpad tap-to-click** (`touchpad_tap`): enable or disable tap-to-click on libinput touchpads (`auto` = libinput default, `true` = enabled, `false` = disabled); applied at device connect, requires a restart
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

- **Window position restore:** each app's last monitor, workspace, position and size are remembered and restored on the next launch (state file under `XDG_STATE_HOME`); terminals are excluded (the configured terminal via `term_app_id`, plus a built-in list of common terminal app_ids), a missing or moved monitor falls back to cascading placement

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

## On-Screen Display (OSD)

- **OSD** for volume, mic and brightness changes: a centered box on the monitor under the cursor shows the label, value (`VOL 65%`, `MIC 30%`, `BRI 40%`, `MUTE` when muted) and a horizontal bar; auto-hides after a configurable timeout (default 1500ms)

## Power Menu

- **Power menu** (`Mod+p`): a centered list of system actions — Suspend, Hibernate, Lock, Log out, Restart, Shut down — driven by keyboard (Up/Down/Enter, or the letter shortcut) or mouse (click a row); each action runs the matching `systemctl`/`loginctl` command

## Lock Screen

- **Lock screen** (`Mod+Shift+l`, or the power menu's Lock action): a full-screen overlay on every monitor showing the desktop wallpaper (dimmed), the current time and date, and password dots as you type. All keyboard and pointer input is consumed — no key or click reaches a client window. The password is verified with PAM (`pam_authenticate`); without PAM at build time it falls back to `loginctl lock-session`. After 5 failed attempts a 30s lockout counts down on screen. The topbar is hidden while locked so no clock/battery/network info is visible. The screen locks automatically on the idle timeout (config `lock_on_idle`) before the screensaver turns the monitors off, so the desktop is never visible without authentication on wake

## Screenshot

- **Screenshot** to PNG in `$XDG_PICTURES_DIR` (or `~/Pictures`), named `guibuxwm-YYYYMMDD-HHMMSS.png`: `Mod+Print` captures the monitor under the cursor, `Mod+Shift+Print` dims the monitor and lets you drag a region (right-click or `Esc` cancels; the dim is not saved), `Mod+Ctrl+Print` captures the focused window
