project('shady-plugin-bad-apple', 'c', default_options: ['c_std=c11', 'warning_level=2'])

cc = meson.get_compiler('c')
# The headers `meson install` of Shady puts in include/shady.
if not cc.has_header('shady/plugin.h')
  error('Shady\'s headers (shady/plugin.h) were not found; install Shady, or use the flake')
endif
xkbcommon = dependency('xkbcommon')
m = cc.find_library('m')
glesv2 = dependency('glesv2')
plugin_dir = join_paths(get_option('libdir'), 'shady', 'plugins')

plugin = shared_module('shady-plugin-bad-apple',
  'bad_apple.c',
  dependencies: [xkbcommon, m, glesv2],
  install: true,
  install_dir: plugin_dir)

# Next to the .so, in the build tree as well as installed.
subdir('shaders/bad-apple')

test('bad-apple-plugin',
  executable('bad-apple-plugin-test', 'tests/bad-apple-plugin.c',
    dependencies: [cc.find_library('dl', required: false), xkbcommon, m],
    install: false),
  args: [plugin.full_path()],
  depends: plugin)
