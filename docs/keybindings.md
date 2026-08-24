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

## Audio

The topbar shows `VOL <pct>%` and `MIC <pct>%` indicators (right side,
next to the network indicator) when an audio system is available
(PulseAudio or PipeWire, via `pactl`). `<pct>%` is replaced by `MUTE`
while muted.

- scroll up/down over an indicator: adjust that volume by 1% per step
- left click: toggle mute
- right click: open the mixer (`pavucontrol`)

Hardware media keys work without any keybind:

| Key | Action |
|---|---|
| `XF86AudioRaiseVolume` | volume-up |
| `XF86AudioLowerVolume` | volume-down |
| `XF86AudioMute` | mute |
| `XF86AudioMicMute` | mic-mute |

## Network

The topbar shows the network status on the right side (SSID for WiFi, interface
name for Ethernet, or "No net"/"NM" when unavailable). It is updated via
NetworkManager D-Bus.

- left click on the indicator: no action
- right click on the indicator: launch `nmtui` in the configured terminal

## Mouse

- `Alt` + left drag on a window: move the window (GNOME-style); the
  window stays where it is dropped. Works on Wayland and X11 windows.
  `Ctrl` held means AltGr, not a plain Alt.
- `Mod` + drag on an X11 window: move the window (X11 windows have no
  titlebar to grab).

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

## Overriding defaults

A config keybind with the same modifiers+key as a default replaces it,
otherwise it is added.

Example — override the default `Mod+e` launcher:

```
keybind = Mod+e: terminal
```

This replaces `Mod+e: launcher` with `Mod+e: terminal`.
`Mod+Shift+e` would remain as the launcher (if bound).
