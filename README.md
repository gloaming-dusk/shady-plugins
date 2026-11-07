# Shady plugins

Plugins for [Shady](https://github.com/gloaming-dusk/shady), the 3D Wayland
compositor, that are not needed by its default rices.

| Plugin | Keys | |
|---|---|---|
| [`bad-apple`](plugins/bad-apple/README.md) | `Super+A` | the screen becomes flip-dot tiles that play a video over the desktop, from your own copy |
| [`black-hole`](plugins/black-hole/README.md) | `Super+H` | the workspace's windows fall into a black hole and a white hole returns them |
| [`frozen-window`](plugins/frozen-window/README.md) | `Super+Z` `Super+Shift+Z` | frost grows over a window, or the whole workspace, and melts away |
| [`magnetic-windows`](plugins/magnetic-windows/README.md) | `Super+M` | nearby windows dock together on springs |
| [`water-windows`](plugins/water-windows/README.md) | `Super+W` | windows turn to water: waves, refraction, caustics |
| [`window-constellation`](plugins/window-constellation/README.md) | `Super+C` | the other windows orbit the focused one |
| [`window-portal`](plugins/window-portal/README.md) | `Super+P` `Super+Shift+P` | a live portal onto another window |
| [`close-burn`](plugins/close-burn/README.md) |  | closing windows burn away |
| [`astral-loom`](plugins/astral-loom/README.md) |  | the sky and glass panels of the retired Astral Loom rice |

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

`tests/run-all.sh <Shady build directory>` builds them all and runs each in a real
compositor, which unit tests against a fake host cannot replace.

See the [C plugin API](https://github.com/gloaming-dusk/shady/blob/main/docs/C_PLUGIN_API.md).
CI builds and unit-tests every plugin against Shady's current `main`.
