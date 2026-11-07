# black-hole

`Super+H` tears open a black hole in the middle of the scene and every window on
the current workspace falls into it. Nothing is closed: press `Super+H` again and
a white hole returns them. Needs Shady's GL renderer (spatial mode).

```lua
-- config.lua
shady.plugins.load("black-hole")
```

To test it against a Shady build:

```bash
tests/headless-black-hole.sh <shady binary> <directory of the plugin's .so>
```
