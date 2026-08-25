# Multi-monitor arrangement

By default monitors are arranged automatically (left to right, in the order
they appear). To place them manually, use the `outputs` config key (see
[Configuration](config.md)) or the `GUIBUX_OUTPUTS` environment variable
(config wins). The `guibuxwm-output` tool changes the layout of a running
compositor without a restart (see [Changing the layout live](#changing-the-layout-live)).

## Format

```
NAME@XxY[:WxH[:ROT]],NAME@XxY,...
```

- `NAME` is the output name reported by wlroots (e.g. `DP-1`, `HDMI-A-1`,
  `eDP-1`). Find yours with `guibuxwm-output list` or in the compositor log.
- `XxY` is the top-left position of the monitor in the virtual layout.
- `:WxH` (optional) sets the video mode (e.g. `1920x1080`). A mode that is
  not available falls back to the monitor's preferred mode.
- `:ROT` (optional) sets the rotation: `normal`, `90`, `180` or `270`
  (degrees clockwise). Example: `HDMI-A-1@0x0:90`.
- `NAME@off` disables the output (its windows are moved to another
  monitor). Re-enable it with `guibuxwm-output enable NAME` or by removing
  the entry.

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

### With a fixed mode

`HDMI-A-1` at 1920x1080:

```sh
GUIBUX_OUTPUTS="HDMI-A-1@0x0:1920x1080" ./build/guibuxwm
```

### Disable a monitor

```sh
GUIBUX_OUTPUTS="DP-1@0x0,HDMI-A-1@off" ./build/guibuxwm
```

## Behavior

- Outputs listed in the spec are placed in the order given.
- Outputs **not** listed are still auto-arranged (left to right, in the order
  wlroots reports them).
- Malformed entries are logged and ignored.
- Up to 8 outputs can be placed manually.
- `NAME@off` keeps the output object alive but out of the layout; its
  windows are moved to another monitor.
- A mode that is not in the monitor's mode list falls back to the
  preferred mode.

## Changing the layout live

The `guibuxwm-output` tool edits the `outputs` line of the config file and
signals the running compositor (SIGUSR1), so the new layout applies without
a restart. Connected outputs (names, positions, current and available
modes) are read from the state file the compositor maintains at
`$XDG_STATE_HOME/guibuxwm/outputs`.

```sh
guibuxwm-output list                          # connected outputs + modes
guibuxwm-output listall                       # all DRM connectors (incl. disconnected)
guibuxwm-output set HDMI-A-1 1920 0           # move (keeps mode/rotation)
guibuxwm-output set HDMI-A-1 0 0 --mode 1920x1080 --transform 90
guibuxwm-output disable HDMI-A-1              # NAME@off
guibuxwm-output enable HDMI-A-1               # back in the layout
guibuxwm-output apply                         # re-apply the config as-is
```

Options: `-c FILE` (config file, default `GUIBUX_CONFIG` or
`~/.config/guibuxwm/config`), `--no-apply` (save without signaling).

## Finding your output names

`guibuxwm-output list` shows the connected outputs and their available
modes. `guibuxwm-output listall` shows all DRM connectors on the system
(connected and disconnected), which is useful for finding the name of a
port you are about to plug in. Alternatively, run the compositor once and
read the output names from the log, or use:

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
