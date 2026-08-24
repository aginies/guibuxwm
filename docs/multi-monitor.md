# Multi-monitor arrangement

By default monitors are arranged automatically (left to right, in the order
they appear). To place them manually, use the `GUIBUX_OUTPUTS` environment variable.

## Format

```
GUIBUX_OUTPUTS="NAME@XxY,NAME@XxY,..."
```

- `NAME` is the output name reported by wlroots (e.g. `DP-1`, `HDMI-A-1`,
  `eDP-1`). Find yours in the compositor log or with `wlr-randr`/`weston-info`.
- `XxY` is the top-left position of the monitor in the virtual layout.
- `:ROT` (optional) sets the rotation: `normal`, `90`, `180` or `270`
  (degrees clockwise). Example: `HDMI-A-1@0x0:90`.

## Examples

### Two monitors side by side

`HDMI-A-1` to the right of `DP-1`:

```sh
GUIBUX_OUTPUTS="DP-1@0x0,HDMI-A-1@1920x0" ./build/guibuxwm
```

### Stacked vertically

```sh
GUIBUX_OUTPUTS="DP-1@0x0,HDMI-A-1@0x1080" ./build/guibuxwm
```

### Portrait monitor on the left

`HDMI-A-1` rotated 90° clockwise, placed at `0x0`:

```sh
GUIBUX_OUTPUTS="HDMI-A-1@0x0:90,DP-8@1440x414" ./build/guibuxwm
```

### Multiple monitors

```sh
GUIBUX_OUTPUTS="eDP-1@0x0,HDMI-A-1@1920x0,DP-1@3840x0" ./build/guibuxwm
```

### With rotation

```sh
GUIBUX_OUTPUTS="DP-1@0x0,HDMI-A-1@0x0:90,DP-8@1920x0:180" ./build/guibuxwm
```

## Behavior

- Outputs listed in `GUIBUX_OUTPUTS` are placed in the order given.
- Outputs **not** listed are still auto-arranged (left to right, in the order
  wlroots reports them).
- Malformed entries are logged and ignored.
- Up to 8 outputs can be placed manually.
- `GUIBUX_OUTPUTS` controls the position and rotation of monitors in the
  virtual layout — the video mode always comes from the monitor's preferred
  mode.

## Finding your output names

Run the compositor once and read the output names from the log, or use:

- `xrandr --query | grep connected` (X11/XWayland)
- `wlr-randr` (under a wlroots compositor)

Example output from `xrandr`:

```
HDMI-1 connected 1440x2560+0+0 right (normal left inverted right x axis y axis)
DP-8 connected primary 2560x1440+1440+414 (normal left inverted right x axis y axis)
```

wlroots uses the full DRM connector names, so `HDMI-1` becomes `HDMI-A-1`.

## Example: Laptop with external portrait monitor

A laptop with an external portrait monitor on the left and a landscape
monitor on the right:

```
HDMI-1 connected 1440x2560+0+0 right (normal left inverted right x axis y axis)
DP-8 connected primary 2560x1440+1440+414 (normal left inverted right x axis y axis)
```

reproduces with:

```sh
GUIBUX_OUTPUTS="HDMI-A-1@0x0:90,DP-8@1440x414" ./build/guibuxwm
```
