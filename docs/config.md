# Configuration

Configuration is via a config file, command-line flags and environment
variables. Priority: command-line flag > config file > `GUIBUX_*` env >
standard env > default.

## Config file

Location: `~/.config/guibuxwm/config` (override with the `-c` flag or the
`GUIBUX_CONFIG` env var). A missing file is skipped silently; malformed lines
are logged and ignored. Format: one `key = value` per line, `#` comments.

A sample with all defaults is in [`config/guibuxwm`](config/guibuxwm) — copy
it to `~/.config/guibuxwm/config` and edit.

## Keys

### `term`

Terminal command started by `Mod+Return`.

**Default:** `gnome-terminal`

**Example:**

```
term = foot
```

---

### `preferred_app1..5`

Preferred apps shown above the command box (`Mod+e`) prompt, up to 5.
Format: `Name;command[;icon]`.

The optional third field is an icon: a theme icon name (e.g. `firefox`)
or an absolute image path. Without it, the command name is used as the
icon name. Icons are shown to the left of the name. A missing theme or
icon is skipped silently (no icon, no error).

Up/Down selects among preferred apps first, then `$PATH` matches.
Enter on a selected preferred app runs its command (typed arguments after
the first word are appended, same as for `$PATH` matches).

**Example:**

```
preferred_app1 = Firefox;firefox
preferred_app2 = Slack;slack;slack-new
preferred_app3 = Nautilus;nautilus
preferred_app4 = VirtUI-GUI;virtui-gui
```

---

### `icon_theme`

Icon theme used to resolve launcher icons (preferred apps and
`.desktop` `Icon=` entries). Leave unset to use the gtk
`gtk-icon-theme-name` from `~/.config/gtk-3.0/settings.ini` (then
gtk-4.0), falling back to `Adwaita`.

Search order: `~/.local/share/icons/<theme>`, each `XDG_DATA_DIRS`
entry + `/share/icons/<theme>`, then `/usr/share/icons/Adwaita` and
`/usr/share/icons/hicolor`. PNG icons only (24x24, 16x16, scalable;
apps and mimetypes contexts). A theme or icon that does not exist is
skipped silently (no icon, no error).

**Default:** gtk `gtk-icon-theme-name`, else `Adwaita`

**Example:**

```
icon_theme = Yaru
```

---

### `xkb_layout`

Keyboard layout (xkb layout name). Leave unset for the system default.

**Default:** system default

**Example:**

```
xkb_layout = fr
```

---

### `xkb_variant`

Keyboard variant. Leave unset for the system default.

**Default:** system default

**Example:**

```
xkb_variant = osd
```

---

### `xkb_options`

xkb options (comma-separated). Leave unset for the system default.

**Default:** system default

**Example:**

```
xkb_options = caps:swapscape
```

---

### `keybind`

Keybinding, repeatable. Syntax: `MODS+key: action`.
At least one modifier is required: `Mod` (Super), `Shift`, `Alt`, `Ctrl`.
`key` is an xkb key name (`q`, `Return`, `Tab`, `F1`, ...).

A config keybind with the same modifiers+key as a default replaces it,
otherwise it is added.

See [Keybindings](../docs/keybindings.md) for the full list of actions.

**Example:**

```
keybind = Mod+g: launcher
keybind = Mod+Shift+q: quit
```

---

### `color_bg`

Topbar/launcher background color.

**Default:** `#1e1e2e`

**Example:**

```
color_bg = #1e1e2e
```

---

### `color_border`

Topbar bottom border / launcher border.

**Default:** `#45475a`

**Example:**

```
color_border = #45475a
```

---

### `color_highlight`

Highlight color (launcher selection, current workspace cell).

**Default:** `#3a3c55`

**Example:**

```
color_highlight = #3a3c55
```

---

### `color_text`

Text and cursor color.

**Default:** `#ffffff`

**Example:**

```
color_text = #ffffff
```

---

### `color_dim`

Dimmed text (launcher non-selected, inactive workspaces).

**Default:** `#8888aa`

**Example:**

```
color_dim = #8888aa
```

---

### `topbar_bg`

Topbar background color.

**Default:** `#73ba25` (openSUSE green)

**Example:**

```
topbar_bg = #73ba25
```

---

### `topbar_text`

Topbar text color (monitor letter, workspaces, date/time).

**Default:** `#1e1e2e`

**Example:**

```
topbar_text = #1e1e2e
```

---

### `topbar_height`

Topbar height in pixels.

**Default:** `24`

**Example:**

```
topbar_height = 28
```

---

### `topbar_font_size`

Topbar font size in pixels.

**Default:** `16`

**Example:**

```
topbar_font_size = 18
```

---

### `topbar_win_pad`

Vertical padding of window pills inside the topbar, in pixels.
`0` = pills fill the whole topbar height.

**Default:** `2`

**Example:**

```
topbar_win_pad = 0
```

---

### `background`

Desktop background image path (PNG or JPEG), used for workspaces
without their own image.

**Default:** unset

**Example:**

```
background = ~/wallpaper.jpg
```

---

### `background1..4`

Per-workspace background image (optional, falls back to `background`).

**Default:** unset

**Example:**

```
background1 = ~/ws1.jpg
background2 = ~/ws2.jpg
background3 = ~/ws3.jpg
background4 = ~/ws4.jpg
```

---

### `background_scale`

Background scale mode: `stretch`, `fit`, `fill`, `tile`.

**Default:** `fill`

**Example:**

```
background_scale = fit
```

---

### `screensaver_timeout`

Screensaver timeout in seconds. `0` = disabled.

**Default:** `300`

**Example:**

```
screensaver_timeout = 600
```

---

### `focus_follow_mouse`

Focus follows mouse: keyboard focus changes when cursor enters a window.

`true` = enabled, `false` = click-to-focus only.

**Default:** `true`

**Example:**

```
focus_follow_mouse = false
```

---

### `overview_workspace_colors`

Overview (F12): show workspace numbers as colored pills on window labels.

**Default:** `false`

**Example:**

```
overview_workspace_colors = true
```

---

### `overview_ws_color1..4`

Overview (F12): per-workspace pill colors.

**Default:** unset (dark pill)

**Example:**

```
overview_ws_color1 = #3a86ff
overview_ws_color2 = #86efac
overview_ws_color3 = #ffb703
overview_ws_color4 = #c084fc
```

---

### `effects`

Master switch for animations (window open/close, notification panel slide).

**Default:** `true`

**Example:**

```
effects = false
```

---

### `effects_duration_ms`

Animation duration in milliseconds. `0` disables animations.

**Default:** `200`

**Example:**

```
effects_duration_ms = 300
```

---

### `window_open_effect`

Animation when a window opens: `scale` (grow from 85% to full size),
`slide` (slide in from the center of its monitor), or `none`.

**Default:** `scale`

**Example:**

```
window_open_effect = slide
```

---

### `notify_effect`

Notification panel animation: `slide` (panel slides in from the right edge)
or `none`.

**Default:** `slide`

**Example:**

```
notify_effect = none
```

---

## Overview (F12)

`F12` shows a GNOME-style overview: every output displays its 4 workspaces
as rows (workspace 1 on top) with the windows of each workspace as
equal-width cells labeled `A1: title` (monitor letter + workspace number).
A semi-transparent dim covers each output.

A workspace column on the left edge of each output lists all four
workspaces (`A1`..`A4`), so empty workspaces stay visible. While a window
is being dragged, the column cell under the cursor is highlighted in the
workspace color — that is where the window will be dropped.

- drag a window onto a row (or its column cell) to move it to that
  workspace, including across monitors
- click an empty area to switch to the workspace of the clicked row
- click a window to select it (switches to its workspace)
- `1`..`4` switch to that workspace, `Esc`/`F12` close the overview

## Notifications

guibuxwm registers as the session-bus notification daemon
(`org.freedesktop.Notifications`, like dunst/mako), so any app that sends
desktop notifications (via `libnotify`/D-Bus) is handled by the compositor
itself. It implements `Notify`, `CloseNotification`, `GetCapabilities`
(`body`) and `GetServerInformation` (spec 1.3).

- **Topbar indicator:** a bell + pending count on each monitor, shown while
  there are unread notifications. Click it to open the panel on that monitor.
- **Panel:** a right-aligned list (up to 10 rows) below the topbar, with a
  "Clear all" button in the header.
  - click a row to focus the window that sent the notification (best-effort
    app-name match, switching to its workspace if needed), dismiss that
    notification and close the panel
  - "Clear all" dismisses everything; the panel closes when empty
  - click empty panel space or press `Esc` to close without dismissing
- **Auto-show:** a new notification pops the panel on the monitor under the
  cursor.
- **Auto-hide:** the panel closes again after 2 seconds unless the cursor is
  over it (hovering keeps it open and restarts the delay).

If there is no session bus, or another daemon already owns the name, the
D-Bus side stays off but the indicator and panel still work for
notifications added internally.

The panel slide animation is controlled by `notify_effect` (and the global
`effects` / `effects_duration_ms` keys).
