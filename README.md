# Shady plugins

Plugins for [Shady](https://github.com/gloaming-dusk/shady), the 3D Wayland
compositor, that are not needed by its default rices.

| Plugin | Keys | |
|---|---|---|
| [`black-hole`](plugins/black-hole/README.md) | `Super+H` | the workspace's windows fall into a black hole and a white hole returns them |

A plugin is native code that runs inside the compositor, with access to your
screen and your keyboard. Read what you install.

## Install

On NixOS, with Shady's module:

```nix
inputs.shady-plugins.url = "github:gloaming-dusk/shady-plugins";

programs.shady.plugins = [ shady-plugins.packages.${pkgs.stdenv.hostPlatform.system}.black-hole ];
```

Elsewhere, with Shady installed so its headers can be found, build it into
Shady's plugin directory for your user:

```bash
cd plugins/black-hole
meson setup build --prefix="$HOME/.local" --libdir=share
meson install -C build
```

Either way, then load it by name in `~/.config/shady/config.lua`:

```lua
shady.plugins.load("black-hole")
```

## Adding a plugin

Each plugin in `plugins/<name>/` is its own Meson project, built only against the
public headers in `include/shady/`:

- `libshady-plugin-<name>.so`, installed in `libdir/shady/plugins`
- its shaders in `shaders/<name>/` next to that file, found with `plugin_path`,
  never through `$SHADY_ROOT`
- `plugin.toml`, `README.md` and tests, with one line in `flake.nix`

See the [C plugin API](https://github.com/gloaming-dusk/shady/blob/main/docs/C_PLUGIN_API.md).
CI builds and tests every plugin against Shady's current `main`.
