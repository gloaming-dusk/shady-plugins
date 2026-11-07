foreach shader : ['overlay.vert', 'astral_sky.frag', 'astral_panel.vert', 'astral_panel.frag']
  configure_file(input: shader, output: shader, copy: true,
    install: true, install_dir: join_paths(plugin_dir, 'shaders', 'astral-loom'))
endforeach
