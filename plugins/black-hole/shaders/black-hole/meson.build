foreach shader : ['black_hole.frag', 'black_hole_window.frag', 'black_hole_window.vert', 'overlay.vert']
  configure_file(input: shader, output: shader, copy: true,
    install: true, install_dir: join_paths(plugin_dir, 'shaders', 'black-hole'))
endforeach
