project('guibuxwm', 'c',
  version: '2.1.0',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=release'],
  meson_version: '>=0.56.0',
)

add_project_arguments('-ffunction-sections', '-fdata-sections', '-march=native', language: 'c')
add_project_link_arguments('-Wl,--gc-sections', language: 'c')

wlroots_dep = dependency('wlroots-0.20')
wayland_dep = dependency('wayland-server')
xkbcommon_dep = dependency('xkbcommon')
freetype_dep = dependency('freetype2')
cairo_dep = dependency('cairo')
dbus_dep = dependency('dbus-1')
threads_dep = dependency('threads')
libinput_dep = dependency('libinput')

# optional: PAM auth for the lock screen; without it the lock falls back
# to `loginctl lock-session`
pam_dep = dependency('pam', required: false)
pam_c_args = pam_dep.found() ? ['-DGUIBUX_HAS_PAM=1'] : ['-DGUIBUX_HAS_PAM=0']

effect_c_args = get_option('effects') ? ['-DGUIBUX_EFFECTS=1'] : ['-DGUIBUX_EFFECTS=0']

# optional: only needed to log which render backend wlroots picked
vulkan_dep = dependency('vulkan', required: false)
vulkan_c_args = vulkan_dep.found() ? ['-DGUIBUX_HAS_VULKAN=1'] : ['-DGUIBUX_HAS_VULKAN=0']

# optional: SVG icon rendering for the topbar indicators (Adwaita symbolic)
rsvg_dep = dependency('librsvg-2.0', required: false)
rsvg_c_args = rsvg_dep.found() ? ['-DGUIBUX_HAS_RSVG=1'] : ['-DGUIBUX_HAS_RSVG=0']

executable('guibuxwm',
  ['src/main.c', 'src/guibuxwm.h', 'src/output.c', 'src/toplevel.c',
    'src/window-layout.c', 'src/topbar.c', 'src/launcher.c', 'src/switcher.c',
    'src/overview.c', 'src/help.c', 'src/outputs-panel.c',
    'src/keyboard.c',
    'src/config.c', 'src/cursor.c', 'src/popup.c', 'tests/wm-test.c',
    'src/sysinfo.c', 'src/background.c', 'src/screensaver.c', 'src/notify.c',
    'src/tooltip.c', 'src/window-preview.c', 'src/osd.c', 'src/power.c', 'src/effects.c', 'src/window-restore.c',
    'src/topbar-items.c', 'src/lock.c',
    'src/outputs-config.c', 'src/outputs-config.h',
    'src/screenshot.c', 'src/stb_image_write.h'],
  c_args: ['-DWLR_USE_UNSTABLE', '-Wno-unused-parameter'] + effect_c_args + vulkan_c_args + rsvg_c_args + pam_c_args,
  include_directories: include_directories('src'),
  dependencies: [wlroots_dep, wayland_dep, xkbcommon_dep, freetype_dep, cairo_dep, dbus_dep, threads_dep, libinput_dep, vulkan_dep, rsvg_dep, pam_dep],
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
