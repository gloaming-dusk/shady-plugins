# glb-loader and glb-wanderer

This directory builds two plugins that share one glTF reader (`gltf.c`):

- `glb-loader` loads a `.glb` file as Shady's 3D environment, with collision.
- [`glb-wanderer`](#glb-wanderer) shows an animated `.glb` character that
  walks about the floor.

## glb-loader

Loads a binary glTF 2.0 file (`.glb`) as Shady's 3D environment: the mesh is
drawn behind the windows, and nodes you mark become collision that windows land
on and bump into.

```lua
-- config.lua
shady.set("environment", true)
shady.set("environment_path", "/home/me/rooms/room.glb")
```

Shady loads `glb-loader` on its own when the path ends in `.glb`, as long as the
plugin is on the plugin search path. `shady.plugins.load("glb-loader")` also
works.

### What is read

The default scene (`scene`, else the first one, else every root node) is
flattened into world space, using the file's coordinates as Shady's: Y up,
one unit per glTF metre. Scale your model, or its root node, to fit the room.

- Node hierarchies with `matrix` or `translation`/`rotation`/`scale`. Mirrored
  nodes (negative determinant) keep their faces pointing outwards.
- Triangles, triangle strips and fans, with 8, 16 or 32-bit indices or none.
  Points and lines are skipped.
- Every accessor form: interleaved `byteStride`, normalized integers, sparse
  accessors, and `KHR_mesh_quantization`.
- Morph targets at their default `weights`, and skins in their rest pose.
- `EXT_mesh_gpu_instancing` and `KHR_node_visibility`.
- Buffers in the `.glb`'s BIN chunk, in `data:` URIs, or in files next to it.
- `NORMAL` and `TEXCOORD_0` when present. Primitives without normals are
  flat-shaded. UVs are passed with V flipped, so their origin is bottom-left like
  OBJ's.

Shady draws the environment with one lit colour, so materials, textures,
animations, cameras and lights are ignored. Files that require such extensions
still load. Files that require Draco or meshopt compression are rejected:
re-export them without mesh compression. Any other required extension is
rejected as well.

### Collision

The visual mesh never collides, because hollow visual geometry would trap
windows. As with the OBJ loader, name a node `collision_<something>`. That
node and its children become exact collision triangles, plus one bounding
box. A world holds 31 boxes besides the floor and 4096 collision triangles,
so keep proxies low-poly. Shady rejects a scene that goes over these limits.

Collision nodes are drawn like any other node. To keep a proxy invisible, add
the custom property `shady_hidden` (Blender: Object Properties → Custom
Properties, exported with *Include → Custom Properties*), or set
`KHR_node_visibility` `visible` to false. Hidden nodes and their children
are not drawn, but still collide.

```json
{ "name": "collision_floor", "mesh": 3, "extras": { "shady_hidden": true } }
```

## glb-wanderer

A rigged character with walk and idle animations wanders about the floor. It
picks a spot, turns, walks there with the walk clip, idles a while, and
picks another, crossfading between the two clips. It is drawn like the
environment, with one lit colour, and replaces a configured environment while
it is shown.

Load it in `config.lua`, and give it a model from `init.lua`. Settings are
published values, because `config.lua` cannot publish. The plugin watches
them, so `shadyctl lua` can change them at any time:

```lua
-- config.lua
shady.plugins.load("glb-wanderer")
```

```lua
-- init.lua
shady.publish("glb-wanderer.model", "/home/me/nixchan/src/assets/nixchan/nixchan-3d.glb")
-- Her beanie only, and none of the things she holds.
shady.publish("glb-wanderer.hide",
    "StrawHat,BucketHat,HairShellBare,HairCrownLocks,Rod,Reel,Can,Hammer")
shady.publish("glb-wanderer.speed", "1.5")
```

| Key (`glb-wanderer.`…) | Default | |
|---|---|---|
| `model` | | the `.glb` to show; unset, nothing is shown |
| `hide` | | node names, comma-separated, not to draw (with their children) |
| `walk`, `idle` | `Walk`, `Idle` | animation names; a missing one leaves the rest pose |
| `height` | `0.6` | her height in world units, in the idle pose |
| `speed` | `1` | walking speed in the model's own units per second. Match it to the walk clip so the feet stay planted (nixchan's is about 1.5) |
| `area` | `-2,-3,2,0` | `x0,z0,x1,z1`: the part of the floor she walks in |
| `fps` | `30` | how often a new pose is handed to Shady |

The model faces +Z, as glTF asks. The scene is rebuilt and uploaded every
frame, about 2 ms of CPU for nixchan's 54,000 triangles.

## Tests

`meson test` loads `.glb` files built byte by byte through a fake host. They
cover the scene graph, every accessor form, primitive modes, morph targets,
skins, instancing, collision, the three buffer sources, and malformed and
randomly corrupted input. A second test drives `glb-wanderer` with a small
animated model: waiting for its model, walking within its area, reloading on
a settings change, and clearing its scene. `tests/headless-glb-loader.sh` and
`tests/headless-glb-wanderer.sh` run each plugin in a real compositor.
