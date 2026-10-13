#!/usr/bin/env python3
"""Write the traced scene's fixture from a scene capture folder, and print the counts trace_scene_test.cpp expects.

python3 make_trace_fixture.py <scene folder> <fixture.bin> [radius_m]

Reads every face's OBJ, as the add-on writes it, back into Unity's frame relative to the first saved face's camera:
(x, y, -z), with each triangle's corners in the order stream output gave them. Keeps the triangles whose three corners
lie within radius_m (default 10) of the camera, without stencil 133, the main character class that holds most of what
is near the camera. Writes them in capture order (the view, then the faces 0 to 5; draws in order) as records of 40
bytes: nine float32 corners, then uint8 face, uint8 kind (0 pre, 1 equal, 2 main), uint8 class (DrawClass in
core/scene.h: pre s128 s129 s133 s136 s137 s144 s165 other) and a zero byte, after a header of b"PTF1" and a uint32
count.

Then it deduplicates them as core/trace_scene.cpp does, and prints what SceneBuilder must count: triangles in, grass
dropped, triangles that collapse on the 1 mm grid, unique triangles, replacements by a higher kind, and unique
triangles still of the prepass.
"""
import glob
import os
import struct
import sys

import numpy as np

CLASSES = ["pre", "s128", "s129", "s133", "s136", "s137", "s144", "s165", "other"]
KINDS = {"pre": 0, "equal": 1, "main": 2}


def read_obj(path):
    """Corners (n, 3, 3) in Unity's frame, kinds (n,) and classes (n,)."""
    vertices, corners, kinds, classes = [], [], [], []
    kind, cls = 2, CLASSES.index("other")
    with open(path, "rb") as f:
        for line in f:
            head = line[:2]
            if head == b"v ":
                x, y, z = line[2:].split()
                vertices.append((float(x), float(y), -float(z)))
            elif head == b"f ":
                a, b, c = line[2:].split()
                # The writer swapped the second and third corners to keep the winding through the mirror.
                corners.append((int(a) - 1, int(c) - 1, int(b) - 1))
                kinds.append(kind)
                classes.append(cls)
            elif head == b"g ":
                kind = KINDS[line.split()[1].decode().split("_")[1]]
            elif line.startswith(b"usemtl"):
                cls = CLASSES.index(line.split()[1].decode())
    v = np.array(vertices, np.float64).reshape(-1, 3)
    t = np.array(corners, np.int64).reshape(-1, 3)
    return v[t], np.array(kinds, np.uint8), np.array(classes, np.uint8)


def key(corners):
    """core/trace_scene.cpp's triangle_key: corners on a 1 mm grid, sorted; None when two corners merge."""
    q = sorted(tuple(int(np.floor(c * 1000.0 + 0.5)) for c in corner) for corner in corners)
    if q[0] == q[1] or q[1] == q[2]:
        return None
    return tuple(q)


def main(folder, out, radius):
    faces = sorted(glob.glob(os.path.join(folder, "face-*-*.obj")), key=lambda p: (0 if "-view." in p else 1, p))
    records = []
    for path in faces:
        face = int(os.path.basename(path).split("-")[1])
        corners, kinds, classes = read_obj(path)
        near = (np.linalg.norm(corners, axis=2).max(axis=1) < radius) & (classes != CLASSES.index("s133"))
        print(f"{os.path.basename(path)}: {len(corners)} triangles, {near.sum()} kept", flush=True)
        for c, k, s in zip(corners[near], kinds[near], classes[near]):
            records.append((c.astype(np.float32), face, int(k), int(s)))
    with open(out, "wb") as f:
        f.write(b"PTF1" + struct.pack("<I", len(records)))
        for c, face, kind, cls in records:
            f.write(c.astype("<f4").tobytes() + bytes((face, kind, cls, 0)))
    print(f"wrote {out}: {len(records)} triangles, {os.path.getsize(out)} bytes")

    grass = CLASSES.index("s129")
    stored = {}
    dropped_class = dropped_degenerate = replaced = 0
    for c, face, kind, cls in records:
        if cls == grass:
            dropped_class += 1
            continue
        k = key(c.astype(np.float64))
        if k is None:
            dropped_degenerate += 1
            continue
        if k not in stored:
            # An equal-depth copy only replaces an earlier copy.
            if kind == 1:
                continue
            stored[k] = kind
        elif kind > stored[k]:
            stored[k] = kind
            replaced += 1
    pre_only = sum(1 for kind in stored.values() if kind == 0)
    print(f"in {len(records)} dropped_class {dropped_class} dropped_degenerate {dropped_degenerate} unique {len(stored)} "
          f"replaced {replaced} pre_only {pre_only}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 10.0)
