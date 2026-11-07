project('shady-plugin-frozen-window', 'c', default_options: ['c_std=c11', 'warning_level=2'])

cc = meson.get_compiler('c')
# The headers `meson install` of Shady puts in include/shady.
if not cc.has_header('shady/plugin.h')
  error('Shady\'s headers (shady/plugin.h) were not found; install Shady, or use the flake')
endif
xkbcommon = dependency('xkbcommon')
m = cc.find_library('m')
plugin_dir = join_paths(get_option('libdir'), 'shady', 'plugins')

plugin = shared_module('shady-plugin-frozen-window',
  'frozen_window.c',
  dependencies: [xkbcommon, m],
  install: true,
  install_dir: plugin_dir)

# Next to the .so, in the build tree as well as installed.
subdir('shaders/frozen-window')

test('frozen-window-plugin',
  executable('frozen-window-plugin-test', 'tests/frozen-window-plugin.c',
    dependencies: [cc.find_library('dl', required: false), xkbcommon, m],
    install: false),
  args: [plugin.full_path()],
  depends: plugin)
