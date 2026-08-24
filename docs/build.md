# Build

## Requirements

Tested on openSUSE. All development packages are expected to be preinstalled:

- `gcc` (or `clang`), `meson`, `ninja`
- `wayland-devel`, `wayland-protocols`
- `libdrm-devel`, `libinput-devel`, `libxkbcommon-devel`
- `freetype-devel`, `cairo-devel`
- `pixman-devel`, `libegl-devel` / `libgles-devel` (Mesa)
- `libudev-devel`, `libdisplay-info-devel`, `lcms2-devel`, `libliftoff-devel`, `libseat-devel`
- `xwayland` (X11 support; the `Xwayland` binary must be on `$PATH`)
- a Wayland terminal (e.g. `gnome-terminal`) started by `Mod+Return`

wlroots itself is **not** taken from the distribution package: `build.sh`
builds wlroots 0.20.2 from source.

## build.sh

```sh
./build.sh
```

`./build.sh clean` removes the `build/` directory (including the binary)
without touching the wlroots installation.

The script:

1. checks for wlroots 0.20.2 via `pkg-config` (module `wlroots-0.20`)
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
