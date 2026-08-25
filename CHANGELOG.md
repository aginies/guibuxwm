# Changelog

Notable changes to guibuxwm, per release.

## [2.0.0] — 2026-08-25

### Added

- **Live monitor layout:** the `outputs` config key now carries per-monitor mode (`WxH`) and rotation (`ROT`), and can be applied to a running compositor without a restart — via the new `guibuxwm-output` command-line tool, `SIGUSR1`, or the `outputs-apply` keybind
- **Outputs panel** (`Mod+m`): edit the monitor layout interactively on screen — list, reorder (left/right swap), mode, rotation, on/off; every change is saved to the `outputs` config line and applied live
- **Battery indicator** (UPower): `BAT NN%` in the topbar with a hover tooltip showing the time estimate (to full / remaining / fully charged)
- **XWayland:** X11 windows (e.g. flatpak apps) map as regular toplevels with focus, tiling, fullscreen, workspaces and topbar entries; `DISPLAY` is set automatically for spawned clients
- **Window position restore:** each app's last monitor, workspace, position and size are remembered and restored on the next launch (state file under `XDG_STATE_HOME`); positions are also saved on a clean WM exit; a missing or replugged monitor falls back to cascading placement
- **`term_app_id` config key:** the configured terminal is excluded from position restore by its actual Wayland app_id (e.g. `gnome-terminal` reports `org.gnome.Terminal`)

### Fixed

- `SIGUSR1` re-apply signal: it was only blocked on the main thread, so the notify/sysinfo worker threads could receive it and the default action terminated the compositor; now blocked process-wide
- Battery: D-Bus properties read with the correct types (uint32/uint16/byte), fully charged is state 3, UPower time estimate read as int64
- Window output tracking across monitors: `toplevel->output` is updated while dragging, and a cross-monitor drag/resize re-resolves the output by position
- `guibuxwm-output`: enabling a monitor whose saved position is taken (or `0x0`) now auto-places it right of the existing layout (extend, not mirror); usage examples added
- Tile test: set the per-workspace tile mode (it set only the active scalar, so the test failed since per-workspace tile modes were introduced)

### Changed

- Version 0.1.0 → 2.0.0

### Docs

- `features.md` completed: window switcher, topbar indicators (network, audio, battery, notifications, window pills), screensaver, outputs panel, media keys and system controls
- `build.md`: full build dependencies and runtime apps (required vs optional, with degrade behavior)
- window position restore and the state file location documented
- README slimmed (130 → 47 lines), features moved to `docs/features.md`

### Project

- `AGENTS.md`: project info, code style and rules for AI agents
- `LICENSE` (MIT)
- removed the "Derived from tinywl" reference
