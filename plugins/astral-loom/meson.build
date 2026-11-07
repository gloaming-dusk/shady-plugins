project('shady-plugin-astral-loom', 'c', default_options: ['c_std=c11', 'warning_level=2'])

cc = meson.get_compiler('c')
# The headers `meson install` of Shady puts in include/shady.
if not cc.has_header('shady/plugin.h')
  error('Shady\'s headers (shady/plugin.h) were not found; install Shady, or use the flake')
endif
xkbcommon = dependency('xkbcommon')
m = cc.find_library('m')
plugin_dir = join_paths(get_option('libdir'), 'shady', 'plugins')

plugin = shared_module('shady-plugin-astral-loom',
  'astral_loom.c',
  dependencies: [xkbcommon, m],
  install: true,
  install_dir: plugin_dir)

# Next to the .so, in the build tree as well as installed.
subdir('shaders/astral-loom')
