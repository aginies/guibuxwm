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

### Reloading

Send `SIGHUP` to the compositor process (`kill -HUP $(pidof guibuxwm)`) or
bind the `reload-config` action to a key (e.g. `keybind = Mod+Shift+c:
reload-config`) to re-read the config file without a restart.

Reloaded live: colors, `topbar_height`/`topbar_font_size`/`topbar_win_pad`/`topbar_items`,
keybinds, `term`/`term_app_id`, backgrounds (`background`, `background1..4`,
`background_scale`), `screensaver_timeout`, `focus_follow_mouse`, `effects*`,
`osd*`, `restore_positions`, `outputs`, overview colors.

Not reloadable (a restart is required, a warning is logged): `renderer`,
`xkb_layout`/`xkb_variant`/`xkb_options`, `icon_theme`, `preferred_app1..5`.

## Keys

### `term`

Terminal command started by `Mod+Return`.

**Default:** `gnome-terminal`

**Example:**

```
term = foot
```

---

### `term_app_id`

Wayland app_id of the terminal, used to exclude it from window position
restore (`restore_positions`). The command name from `term` is not always
the app_id the terminal reports (GNOME Terminal runs as `gnome-terminal`
but reports `org.gnome.Terminal`), so set this key when the exclusion does
not match. Without it, the basename of the first word of `term` is used.

**Default:** derived from `term`

**Example:**

```
term_app_id = org.gnome.Terminal
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
preferred_app1 = Firefox;firefox;firefox
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

**Default:** `#8839ef` (openSUSE green)

**Example:**

```
topbar_bg = #8839ef
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

### `topbar_items`

Right-side topbar indicators, comma-separated, in layout order.
Available items: `network`, `volume`, `mic`, `battery`,
`notifications`, `clock`. Items not listed are not rendered (and
their click/scroll/tooltip handlers are inactive). An item that is
enabled but has no data (e.g. no battery, no audio) stays hidden
automatically.

**Default:** `network, volume, mic, battery, notifications, clock`

**Example:**

```
topbar_items = network, battery, clock
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

### `restore_positions`

Remember each app's last monitor, workspace, position and size and restore
them on the next launch. The terminal is excluded (see `term_app_id`).

The state is a plain text file, one line per app
(`app_id|output|box_x|box_y|workspace|x|y|w|h`), stored at
`$XDG_STATE_HOME/guibuxwm/window-positions` (or
`~/.local/state/guibuxwm/window-positions` when `XDG_STATE_HOME` is unset).
`box_x`/`box_y` is the monitor's position in the virtual layout: a monitor
that reappears under the same name but at a different position is treated
as a different (replugged) physical monitor and does not receive the
restored window. Positions are saved when a window closes and on a clean
compositor exit; a missing monitor or an off-screen position falls back to
normal cascading placement.

**Default:** `true`

**Example:**

```
restore_positions = false
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

### `osd`

On-screen display for volume, mic and brightness changes: a centered box
on the monitor under the cursor shows the label, value (`VOL 65%`,
`MIC 30%`, `BRI 40%`, `MUTE` when muted) and a horizontal bar.

**Default:** `true`

**Example:**

```
osd = false
```

---

### `osd_timeout_ms`

How long the OSD stays visible after a change, in milliseconds. `0`
disables the OSD.

**Default:** `1500`

**Example:**

```
osd_timeout_ms = 2000
```

---

### `outputs`

Monitor arrangement spec, see [Multi-monitor](multi-monitor.md).
Entries: `NAME@XxY[:WxH[:ROT]]` (place at XxY, optional mode, optional
rotation) or `NAME@off` (disable). `auto` or unset = arrange every
connected output automatically. Wins over the `GUIBUX_OUTPUTS` env var.

The `guibuxwm-output` tool edits this line while the compositor runs
and signals a re-apply, so changes take effect without a restart.

**Default:** unset (auto-arrange)

**Example:**

```
outputs = DP-1@0x0,HDMI-A-1@1920x1080:1920x1080:90
```

---

### `renderer`

Render backend used by wlroots: `auto` (let wlroots pick), `gles2`,
`vulkan`, or `pixman` (software, fallback only). On wlroots 0.20, `auto`
prefers GLES2; `vulkan` is the fastest option on modern GPUs and the
only GPU backend in wlroots 0.21+. If the requested backend cannot be
created, the compositor exits — remove the line to fall back to `auto`.
An explicit `WLR_RENDERER` env var always wins over this key. The active
backend is logged at startup (`renderer: vulkan|gles2|pixman`).

**Default:** `auto`

**Example:**

```
renderer = vulkan
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
