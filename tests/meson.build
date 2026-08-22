# Test clients for guibuxwm. Each maps real toplevels against a compositor
# started with the matching GUIBUX_TEST_* hook; the compositor log is the
# verdict. Built as part of the main build (ninja -C build).
wayland_client_dep = dependency('wayland-client')

test_deps = [wayland_client_dep]

executable('ws-test',
  'ws-test.c',
  'xdg-shell-protocol.c',
  dependencies: test_deps,
)

executable('tile-test',
  'tile-test.c',
  'xdg-shell-protocol.c',
  dependencies: test_deps,
)
