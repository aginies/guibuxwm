# Keybindings

Keybindings use the syntax `MODS+key: action`.

## Modifiers

At least one modifier is required:

| Modifier | Key |
|---|---|
| `Mod` | Super key (Windows key) |
| `Shift` | Shift |
| `Alt` | Alt |
| `Ctrl` | Ctrl |

## Key names

`key` is an xkb key name. Common examples:

- `q`, `w`, `e`, `r`, `t`, `y`, `u`, `i`, `o`, `p`
- `Return`, `Escape`, `Tab`, `Space`
- `Left`, `Right`, `Up`, `Down`
- `F1` .. `F12`

## Actions

| Action | Meaning |
|---|---|
| `terminal` | Start a new terminal |
| `close` | Close the focused window |
| `fullscreen` | Toggle fullscreen of the focused window (`Mod+f` or `Mod+Up`) |
| `tile` | Cycle tile mode of the focused window's monitor (free → split → main+stack) |
| `launcher` | Open the command box (`Mod+e`) |
| `focus-next` | Cycle window focus |
| `quit` | Quit the compositor |
| `workspace:N` | Switch to workspace N (1..4) |
| `move-workspace:N` | Move the focused window to workspace N (1..4) |
| `move-monitor-left` | Move the focused window to the previous monitor |
| `move-monitor-right` | Move the focused window to the next monitor |
| `snap-left` | Snap the focused window to the left half of its monitor |
| `snap-right` | Snap the focused window to the right half of its monitor |
| `snap-top` | Snap the focused window to the top half of its monitor |
| `snap-bottom` | Snap the focused window to the bottom half of its monitor |
| `switch-ws-left` | Switch to the previous workspace |
| `switch-ws-right` | Switch to the next workspace |
| `show-help` | Show the keybinding help overlay |
| `volume-up` | Raise default sink volume by 5% |
| `volume-down` | Lower default sink volume by 5% |
| `mute` | Toggle default sink mute |
| `mic-up` | Raise default source (mic) volume by 5% |
| `mic-down` | Lower default source (mic) volume by 5% |
| `mic-mute` | Toggle default source (mic) mute |

## Default Keybindings

| Shortcut | Action |
|---|---|
| `Mod+Return` | Start a new terminal |
| `Mod+e` | Command box: type a command, Enter runs it via `sh -c` |
| `Mod+q` | Close focused window |
| `Mod+f` | Toggle fullscreen |
| `Mod+t` | Cycle tile mode of the focused window's monitor (free / split / main+stack) |
| `Alt+Tab` | Cycle window focus |
| `F12` | Toggle GNOME-style overview (all workspaces + windows) |
| `Mod+1..4` | Switch to workspace 1..4 on the focused window's monitor |
| `Mod+Shift+1..4` | Move focused window to workspace 1..4 (same monitor) |
| `Mod+Ctrl+Left` / `Mod+Ctrl+Right` | Switch to previous/next workspace |
| `Mod+Shift+Left` / `Mod+Shift+Right` | Move focused window to previous/next monitor |
| `Mod+Left` | Snap focused window to left half of its monitor |
| `Mod+Right` | Snap focused window to right half of its monitor |
| `Mod+Ctrl+Shift+Up` / `Mod+Ctrl+Shift+Down` | Snap focused window to top/bottom half of its monitor |
| `Mod+Up` | Fullscreen focused window |
| `Mod+h` | Show the keybinding help overlay |
| `Mod+Shift+q` | Quit |
| `Alt+Escape` | Quit |
| `Mod+Alt+Escape` | Quit |

## Mouse

- `Alt` + left drag on a window: move the window (GNOME-style); the window stays where it is dropped. Works on Wayland and X11 windows. `Ctrl` held means AltGr, not a plain Alt.
- `Mod` + drag on an X11 window: move the window (X11 windows have no titlebar to grab).
- Click a window to focus it, drag its titlebar to move, drag its edges to resize. Dragging or resizing a fullscreen window leaves fullscreen.

## Audio

The topbar shows `VOL <pct>%` and `MIC <pct>%` indicators (right side, next to the network indicator) when an audio system is available (PulseAudio or PipeWire, via `pactl`). `<pct>%` is replaced by `MUTE` while muted.

- scroll up/down over an indicator: adjust that volume by 1% per step
- left click: toggle mute
- right click: open the mixer (`pavucontrol`)

Hardware media keys work without any keybind:

| Key | Action |
|---|---|
| `XF86AudioRaiseVolume` | Raise volume by 5% |
| `XF86AudioLowerVolume` | Lower volume by 5% |
| `XF86AudioMute` | Toggle mute |
| `XF86AudioMicMute` | Toggle mic mute |

## Network

The topbar shows the network status on the right side (SSID for WiFi, interface name for Ethernet, or "No net"/"NM" when unavailable). Updated via NetworkManager D-Bus.

- left click: no action
- right click: launch `nmtui` in the configured terminal

## Command Box (`Mod+e`)

Opens an input box centered on the monitor under the cursor. Type a command and press Enter to run it via `/bin/sh -c` (so pipes, redirections and environment variables work). Escape or a mouse click dismisses it without running anything.

As you type, matching commands from `$PATH` are listed below the input line (up to 8, exact matches first). Navigate with Up/Down; Enter runs the selected command, with any arguments you typed after the first word appended (e.g. type `alac -w`, select `alacritty`, Enter runs `alacritty -w`). With no match, Enter runs exactly what you typed.

Preferred apps configured with `preferred_app1..5` (see [Config](config.md#preferred-apps)) are always listed above the input line, up to 5. Each preferred app can optionally include an icon (`Name;command;icon-name`). Icons are resolved from the configured `icon_theme` (or the system GTK icon theme, falling back to Adwaita) and drawn to the left of the app name. Matches from `$PATH` and `.desktop` files also show their icons when available.

With nothing selected, the first Up selects the preferred app closest to the input line; further Up/Down moves through the preferred apps and the matches. Enter on a selected preferred app runs its command (typed arguments after the first word are appended, same as for matches).

## Overview (F12)

`F12` shows a GNOME-style overview: every output displays its 4 workspaces as rows (workspace 1 on top) with the windows of each workspace as equal-width cells labeled `A1: title` (monitor letter + workspace number). A semi-transparent dim covers each output.

A workspace column on the left edge of each output lists all four workspaces (`A1`..`A4`), so empty workspaces stay visible. While a window is being dragged, the column cell under the cursor is highlighted in the workspace color — that is where the window will be dropped.

- drag a window onto a row (or its column cell) to move it to that workspace, including across monitors
- click an empty area to switch to the workspace of the clicked row
- click a window to select it (switches to its workspace)
- `1`..`4` switch to that workspace, `Esc`/`F12` close the overview

## Notifications

guibuxwm registers as the session-bus notification daemon (`org.freedesktop.Notifications`, like dunst/mako), so any app that sends desktop notifications (via `libnotify`/D-Bus) is handled by the compositor itself.

- **Topbar indicator:** a bell + pending count on each monitor, shown while there are unread notifications. Click it to open the panel on that monitor.
- **Panel:** a right-aligned list (up to 10 rows) below the topbar, with a "Clear all" button in the header.
  - click a row to focus the window that sent the notification, dismiss that notification and close the panel
  - "Clear all" dismisses everything; the panel closes when empty
  - click empty panel space or press `Esc` to close without dismissing
- **Auto-show:** a new notification pops the panel on the monitor under the cursor.
- **Auto-hide:** the panel closes again after 2 seconds unless the cursor is over it (hovering keeps it open and restarts the delay).

If there is no session bus, or another daemon already owns the name, the D-Bus side stays off but the indicator and panel still work for notifications added internally.

The panel slide animation is controlled by `notify_effect` (and the global `effects` / `effects_duration_ms` keys).

## Tile Modes (`Mod+t`)

`Mod+t` cycles the tile mode of the monitor holding the focused window. Each monitor keeps its own mode.

| Mode | Layout |
|---|---|
| free | default: cascading placement, free move/resize |
| split | windows fill two 50% columns round-robin, stacked vertically within a column |
| main+stack | focused window takes the left 50%, remaining windows stack vertically in the right 50% |

In tile modes, new windows are placed into the layout (instead of cascading), closing a window repacks the remaining ones, and leaving fullscreen returns the window to its slot. Dragging or resizing a window takes it out of the layout until the next retiling (`Mod+t`, window map/close, fullscreen off, or moving a window between monitors).

## Overriding Defaults

A config keybind with the same modifiers+key as a default replaces it, otherwise it is added.

Example — override the default `Mod+e` launcher:

```
keybind = Mod+e: terminal
```

This replaces `Mod+e: launcher` with `Mod+e: terminal`. `Mod+Shift+e` would remain as the launcher (if bound).

## Examples

```
# Launch terminal with Mod+Return
keybind = Mod+Return: terminal

# Close focused window with Mod+q
keybind = Mod+q: close

# Toggle fullscreen with Mod+f or Mod+Up
keybind = Mod+f: fullscreen

# Cycle tile mode with Mod+t
keybind = Mod+t: tile

# Open command box with Mod+e
keybind = Mod+e: launcher

# Cycle window focus with Alt+Tab
keybind = Alt+Tab: focus-next

# Switch workspaces with Mod+1..4 and Mod+Shift+1..4
keybind = Mod+1: workspace:1
keybind = Mod+2: workspace:2
keybind = Mod+3: workspace:3
keybind = Mod+4: workspace:4
keybind = Mod+Shift+1: move-workspace:1
keybind = Mod+Shift+2: move-workspace:2
keybind = Mod+Shift+3: move-workspace:3
keybind = Mod+Shift+4: move-workspace:4

# Move focused window between monitors
keybind = Mod+Shift+Left: move-monitor-left
keybind = Mod+Shift+Right: move-monitor-right

# Snap focused window to half of its monitor
keybind = Mod+Left: snap-left
keybind = Mod+Right: snap-right
keybind = Mod+Shift+Up: snap-top
keybind = Mod+Shift+Down: snap-bottom
keybind = Mod+Ctrl+Shift+Up: snap-top
keybind = Mod+Ctrl+Shift+Down: snap-bottom

# Switch workspaces with Ctrl+Alt+Left/Right
keybind = Ctrl+Alt+Left: switch-ws-left
keybind = Ctrl+Alt+Right: switch-ws-right

# Show the keybinding help overlay with Mod+h
keybind = Mod+h: show-help

# Quit with Mod+Shift+q, Alt+Escape, or Mod+Alt+Escape
keybind = Mod+Shift+q: quit
keybind = Alt+Escape: quit
keybind = Mod+Alt+Escape: quit
```
