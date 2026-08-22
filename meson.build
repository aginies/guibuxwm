project('guibuxwm', 'c',
  version: '0.1.0',
  default_options: ['c_std=gnu11', 'warning_level=2'],
)

wlroots_dep = dependency('wlroots-0.20')
wayland_dep = dependency('wayland-server')
xkbcommon_dep = dependency('xkbcommon')
freetype_dep = dependency('freetype2')
cairo_dep = dependency('cairo')
dbus_dep = dependency('dbus-1')

executable('guibuxwm',
  ['src/main.c', 'src/guibuxwm.h', 'src/output.c', 'src/toplevel.c',
    'src/window-layout.c', 'src/topbar.c', 'src/launcher.c', 'src/keyboard.c',
    'src/config.c', 'src/cursor.c', 'src/popup.c', 'src/wm-test.c',
    'src/sysinfo.c'],
  c_args: ['-DWLR_USE_UNSTABLE', '-Wno-unused-parameter'],
  dependencies: [wlroots_dep, wayland_dep, xkbcommon_dep, freetype_dep, cairo_dep, dbus_dep],
  install: true,
)

subdir('tests')
