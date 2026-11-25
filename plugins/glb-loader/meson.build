project('shady-plugin-glb-loader', 'c', default_options: ['c_std=c11', 'warning_level=2'])

cc = meson.get_compiler('c')
# The headers `meson install` of Shady puts in include/shady.
if not cc.has_header('shady/environment.h')
  error('Shady\'s headers (shady/environment.h) were not found; install Shady, or use the flake')
endif
m = cc.find_library('m')
plugin_dir = join_paths(get_option('libdir'), 'shady', 'plugins')

# Both plugins read glTF with gltf.c.
gltf = static_library('gltf', 'gltf.c', dependencies: [m], pic: true)

# Named <extension>-loader, so Shady loads it for a configured .glb environment.
plugin = shared_module('shady-plugin-glb-loader',
  'glb_loader.c',
  link_with: gltf,
  dependencies: [m],
  install: true,
  install_dir: plugin_dir)

wanderer = shared_module('shady-plugin-glb-wanderer',
  'glb_wanderer.c',
  link_with: gltf,
  dependencies: [m],
  install: true,
  install_dir: plugin_dir)

test('glb-loader-plugin',
  executable('glb-loader-plugin-test', 'tests/glb-loader-plugin.c',
    dependencies: [cc.find_library('dl', required: false), m],
    install: false),
  args: [plugin.full_path()],
  depends: plugin)

test('glb-wanderer-plugin',
  executable('glb-wanderer-plugin-test', 'tests/glb-wanderer-plugin.c',
    dependencies: [cc.find_library('dl', required: false), m],
    install: false),
  args: [wanderer.full_path()],
  depends: wanderer)
