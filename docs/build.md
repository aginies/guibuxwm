# Build

## Build requirements

Tested on openSUSE. Build tools:

- `gcc` (or `clang`), `meson`, `ninja`, `curl` (wlroots tarball download)

Development packages (openSUSE names):

- **guibuxwm itself:** `wayland-devel` (server + client, the client is
  needed for the test binaries), `libxkbcommon-devel`, `freetype-devel`,
  `cairo-devel`, `dbus-1-devel`
- **wlroots 0.20.2** (built from source by `build.sh`, see below):
  `wayland-devel`, `wayland-protocols`, `libdrm-devel`, `libinput-devel`,
  `libxkbcommon-devel`, `freetype-devel`, `cairo-devel`, `pixman-devel`,
  `libegl-devel` / `libgles-devel` (Mesa), `libudev-devel`,
  `libdisplay-info-devel`, `lcms2-devel`, `libliftoff-devel`,
  `libseat-devel`, `xwayland`
- `xwayland` (X11 support; the `Xwayland` binary must be on `$PATH`)

wlroots is **not** taken from the distribution package: `build.sh` builds
wlroots 0.20.2 from source with XWayland enabled.

### Meson options

| Option | Default | Meaning |
|---|---|---|
| `effects` | `true` | Compile in window/workspace/notification animations (can also be disabled at runtime via the `effects` config key) |

```sh
meson setup build -Deffects=false   # no animations at all
```

## build.sh

```sh
./build.sh
```

`./build.sh clean` removes the `build/` directory (including the binary)
without touching the wlroots installation. Other modes: `debug`, `release`,
`sanitize` (ASan+UBSan), `verbose`.

The script:

1. checks for wlroots 0.20.2 via `pkg-config` (module `wlroots-0.20`,
   XWayland enabled)
2. if missing: downloads the [wlroots 0.20.2 tarball](https://gitlab.freedesktop.org/wlroots/wlroots/-/archive/0.20.2/wlroots-0.20.2.tar.gz),
   builds it and installs it to `~/.local` (override with `WLR_PREFIX`)
3. runs `meson setup build` + `ninja -C build`

### `WLR_PREFIX`

Override the wlroots install prefix:

```sh
WLR_PREFIX=$HOME/.local ./build.sh
```

## Manual build

If wlroots is already installed:

```sh
export PKG_CONFIG_PATH=$HOME/.local/lib64/pkgconfig:$PKG_CONFIG_PATH
meson setup build
ninja -C build
```

## Run

```sh
./build/guibuxwm
```

## Runtime requirements

Apps and services the compositor needs at runtime. Missing optional pieces
degrade gracefully: the affected indicator is hidden or the action is a
no-op, the compositor keeps working.

### Required

| What | Why |
|---|---|
| a Wayland terminal | started by `Mod+Return` and by the network right-click; configured with `term` (default: `gnome-terminal`) |
| `Xwayland` binary on `$PATH` | X11 apps (e.g. flatpak) |
| a monospace font at `/usr/share/fonts/truetype/LiberationMono-Regular.ttf` or `/usr/share/fonts/truetype/SUSEMono-Regular.otf` | topbar, launcher, switcher and panel text (FreeType) |
| `/bin/sh` | all spawned commands run through it |

### Optional

| App / service | Used for | If missing |
|---|---|---|
| `pactl` (PulseAudio or PipeWire) | `VOL`/`MIC` topbar indicators, volume/mic keybinds and media keys | no audio indicators, volume keybinds are no-ops |
| `pavucontrol` | mixer, right-click on an audio indicator | right-click is a no-op |
| `nmtui` (NetworkManager package) | network configuration, right-click on the network indicator (opened in the terminal) | right-click is a no-op |
| `brightnessctl` | brightness keybinds and media keys | brightness actions are no-ops |
| NetworkManager (`org.freedesktop.NetworkManager` on the session bus) | network status indicator (SSID / interface name) | indicator shows "No net" / "NM" |
| UPower (`org.freedesktop.UPower` on the session bus) | battery indicator and hover tooltip (percentage, time estimate) | no battery indicator |
| a D-Bus session bus | notification daemon (`org.freedesktop.Notifications`), the indicators above | notifications still work internally (panel + indicator), no external apps can send notifications |
| `xdg-desktop-portal` (+ `gnome-terminal-server` if the terminal is GNOME Terminal) | clicking a URL in an app opens the default browser (the compositor imports `DISPLAY`/`WAYLAND_DISPLAY` into the systemd user manager and restarts the portal services) | URL clicks may not open a browser |
| an icon theme (configured with `icon_theme`, falls back to the GTK theme, then Adwaita) | launcher and preferred-app icons | entries show without icons |
| background images (PNG/JPEG) | desktop background, per workspace | plain `color_bg` background |
