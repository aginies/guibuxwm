project('guibuxwm', 'c',
  version: '2.0.0',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=release'],
  meson_version: '>=0.56.0',
)

add_project_arguments('-ffunction-sections', '-fdata-sections', language: 'c')
add_project_link_arguments('-Wl,--gc-sections', language: 'c')

wlroots_dep = dependency('wlroots-0.20')
wayland_dep = dependency('wayland-server')
xkbcommon_dep = dependency('xkbcommon')
freetype_dep = dependency('freetype2')
cairo_dep = dependency('cairo')
dbus_dep = dependency('dbus-1')
threads_dep = dependency('threads')

effect_c_args = get_option('effects') ? ['-DGUIBUX_EFFECTS=1'] : ['-DGUIBUX_EFFECTS=0']

executable('guibuxwm',
  ['src/main.c', 'src/guibuxwm.h', 'src/output.c', 'src/toplevel.c',
    'src/window-layout.c', 'src/topbar.c', 'src/launcher.c', 'src/switcher.c',
    'src/overview.c', 'src/help.c', 'src/outputs-panel.c',
    'src/keyboard.c',
    'src/config.c', 'src/cursor.c', 'src/popup.c', 'tests/wm-test.c',
    'src/sysinfo.c', 'src/background.c', 'src/screensaver.c', 'src/notify.c',
    'src/tooltip.c', 'src/effects.c', 'src/window-restore.c',
    'src/outputs-config.c', 'src/outputs-config.h'],
  c_args: ['-DWLR_USE_UNSTABLE', '-Wno-unused-parameter'] + effect_c_args,
  include_directories: include_directories('src'),
  dependencies: [wlroots_dep, wayland_dep, xkbcommon_dep, freetype_dep, cairo_dep, dbus_dep, threads_dep],
  install: true,
)

# standalone monitor layout tool: edits the config `outputs` line and
# signals the running compositor (SIGUSR1) to re-apply it
executable('guibuxwm-output',
  ['tools/guibuxwm-output.c', 'src/outputs-config.c', 'src/outputs-config.h'],
  include_directories: include_directories('src'),
  install: true,
)

subdir('tests')
