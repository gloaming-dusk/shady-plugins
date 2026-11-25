#!/usr/bin/env python3
"""Write Shady's test room (assets/test-room.obj) as a .glb: the wedge as one
node, its collision proxy as a hidden collision_room node.

    make-test-room.py <out.glb>
"""
import json
import struct
import sys

# The wedge: 8 corners, 10 triangles (OBJ indices minus one).
corners = [
    (-0.75, -0.62, -1.60), (0.75, -0.62, -1.60), (0.75, -0.62, -0.40), (-0.75, -0.62, -0.40),
    (-0.75, 0.10, -1.60), (0.75, 0.10, -1.60), (0.75, -0.20, -0.40), (-0.75, -0.20, -0.40),
]
visual = [(0, 1, 4), (4, 1, 5), (1, 2, 5), (5, 2, 6), (2, 3, 6), (6, 3, 7),
          (3, 0, 7), (7, 0, 4), (4, 5, 7), (7, 5, 6)]
proxy = visual + [(3, 2, 1), (3, 1, 0)]  # closed with the bottom

positions = b"".join(struct.pack("<3f", *c) for c in corners)
lo = [min(c[i] for c in corners) for i in range(3)]
hi = [max(c[i] for c in corners) for i in range(3)]


def indices(tris):
    data = b"".join(struct.pack("<3H", *t) for t in tris)
    return data + b"\0" * (-len(data) % 4)


vis, col = indices(visual), indices(proxy)
binary = positions + vis + col
doc = {
    "asset": {"version": "2.0", "generator": "shady-plugins make-test-room.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1]}],
    "nodes": [
        {"name": "room", "mesh": 0},
        {"name": "collision_room", "mesh": 1, "extras": {"shady_hidden": True}},
    ],
    "meshes": [
        {"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]},
        {"primitives": [{"attributes": {"POSITION": 0}, "indices": 2}]},
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 8, "type": "VEC3", "min": lo, "max": hi},
        {"bufferView": 1, "componentType": 5123, "count": len(visual) * 3, "type": "SCALAR"},
        {"bufferView": 2, "componentType": 5123, "count": len(proxy) * 3, "type": "SCALAR"},
    ],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(positions), "target": 34962},
        {"buffer": 0, "byteOffset": len(positions), "byteLength": len(visual) * 6, "target": 34963},
        {"buffer": 0, "byteOffset": len(positions) + len(vis), "byteLength": len(proxy) * 6,
         "target": 34963},
    ],
    "buffers": [{"byteLength": len(binary)}],
}
text = json.dumps(doc, separators=(",", ":")).encode()
text += b" " * (-len(text) % 4)
with open(sys.argv[1], "wb") as f:
    f.write(struct.pack("<3I", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(binary)))
    f.write(struct.pack("<2I", len(text), 0x4E4F534A) + text)
    f.write(struct.pack("<2I", len(binary), 0x004E4942) + binary)
