# AGENTS.md

Instructions for AI agents working on guibuxwm.

## Project

Wayland window compositor built on wlroots 0.20. Derived from tinywl. C99, meson build.

## Build

```sh
./build.sh          # build
./build.sh clean    # clean build/
```

Manual:

```sh
export PKG_CONFIG_PATH=$HOME/.local/lib64/pkgconfig:$PKG_CONFIG_PATH
meson setup build
ninja -C build
```

Run: `./build/guibuxwm`

## Code Style

- C99, 4-space indent, no tabs
- snake_case for functions/variables, UPPER_CASE for macros/defines
- `#include` order: system headers, then project headers, alphabetically within each group
- Comments explain why, not what
- Error paths: log + goto cleanup pattern
- Public API in `.h` files, static functions in `.c` files
- No global mutable state unless intentional (config, singletons)

## Key Files

| File | Purpose |
|---|---|
| `src/main.c` | Entry point, wlroots context setup |
| `src/config.c` | Config file parsing, defaults |
| `src/toplevel.c` | XDG/X11 surface lifecycle, focus |
| `src/window-layout.c` | Tiling modes (free/split/main+stack) |
| `src/topbar.c` | Per-monitor topbar rendering |
| `src/overview.c` | F12 overview mode |
| `src/launcher.c` | Command box (`Mod+e`) |
| `src/keyboard.c` | Keybind dispatch |
| `src/notify.c` | Notification daemon |
| `src/output.c` | Monitor handling, arrangement |
| `src/effects.c` | Window/panel animations |
| `src/background.c` | Desktop background rendering |
| `src/window-restore.c` | Position save/restore |
| `src/sysinfo.c` | Audio/network status polling |
| `src/cursor.c` | Cursor shape, grab state |
| `src/help.c` | Keybinding help overlay |
| `src/screensaver.c` | Idle inhibitor |
| `src/guibuxwm.h` | Shared types, forward declarations |

## Testing

```sh
tests/run-all.sh          # all tests
tests/run-<name>-test.sh  # individual
```

Headless smoke test:

```sh
WLR_BACKENDS=headless WLR_RENDERER=gles2 ./build/guibuxwm
```

## Config

Location: `~/.config/guibuxwm/config`
Sample: `config/guibuxwm`

Format: `key = value`, `#` comments. One per line.

## Docs

- `docs/features.md` — feature list
- `docs/keybindings.md` — keybindings reference
- `docs/build.md` — build instructions
- `docs/config.md` — config key reference
- `docs/testing.md` — test details
- `docs/multi-monitor.md` — monitor arrangement
- `docs/project-layout.md` — this file's purpose

## Conventions

- New feature: add to `docs/features.md`, update README if user-facing
- New config key: add to `docs/config.md`, `config/guibuxwm` sample
- New keybind action: add to `docs/keybindings.md` actions table
- Commit messages: imperative, lowercase, no period
- PRs: describe what changed, why, link to issue if applicable
