foreach shader : ['overlay.vert', 'bad_apple.frag']
  configure_file(input: shader, output: shader, copy: true,
    install: true, install_dir: join_paths(plugin_dir, 'shaders', 'bad-apple'))
endforeach
