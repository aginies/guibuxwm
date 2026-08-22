project('guibuxwm', 'c',
  version: '0.1.0',
  default_options: ['c_std=gnu11', 'warning_level=2'],
)

wlroots_dep = dependency('wlroots-0.20')
wayland_dep = dependency('wayland-server')
xkbcommon_dep = dependency('xkbcommon')
freetype_dep = dependency('freetype2')
cairo_dep = dependency('cairo')

executable('guibuxwm',
  'src/main.c',
  c_args: ['-DWLR_USE_UNSTABLE', '-Wno-unused-parameter'],
  dependencies: [wlroots_dep, wayland_dep, xkbcommon_dep, freetype_dep, cairo_dep],
  install: true,
)
