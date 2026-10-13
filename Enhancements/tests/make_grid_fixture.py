#!/usr/bin/env python3
"""Cut the consistency grid's fixture from a Deck scene folder and a trace folder saved at the same spot.

python3 make_grid_fixture.py <scene folder> <trace folder> <fixture.bin> [view_m] [radius_m]

Keeps the consistency grid's pixels of the trace folder's frame (every 16th pixel each way, a world stencil that is not
grass, vegetation or foliage, with depth) whose surface lies less than view_m (default 15) in front of the camera, and
the scene's world and other triangles (the classes a ray from the camera sees) with a corner within radius_m (default
25) of the camera. Writes b"PGF1", the uint32 counts of pixels and triangles, the trace folder's pushed camera
(world_to_view and view_to_clip, 16 float32 each in Unity's order, as trace.tsv has them), the scene's reference
(3 float64), the pixels (uint16 x, uint16 y, float32 stored depth, uint8 stencil, 3 zero bytes) and the triangles as
trace-scene.bin holds them (nine float32 corners relative to the reference and the uint32 info).

Build 202's view-only capture scene-20261011-221045 and the self-test's save trace-20261011-221018, at the same spot,
make tests/fixtures/scene-221045-grid.bin, which trace_test.cpp checks.
"""
import struct
import sys
from pathlib import Path

import numpy as np

WIDTH, HEIGHT, STRIDE = 1920, 1200, 16
SOLID = {0, 3}  # TraceClass world and other


def main():
    scene, trace, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    view_m = float(sys.argv[4]) if len(sys.argv) > 4 else 15.0
    radius = float(sys.argv[5]) if len(sys.argv) > 5 else 25.0

    values = dict(line.rstrip("\n").split("\t", 1) for line in open(trace / "trace.tsv") if "\t" in line)
    world_to_view = [float(v) for v in values["world_to_view"].split(",")]
    view_to_clip = [float(v) for v in values["view_to_clip"].split(",")]
    # Unity's column-major order: element (row r, column c) at c * 4 + r.
    w2v = np.array(world_to_view, dtype=np.float64).reshape(4, 4).T
    v2c = np.array(view_to_clip, dtype=np.float64).reshape(4, 4).T

    data = (scene / "trace-scene.bin").read_bytes()
    assert data[:4] == b"PRTS"
    count = struct.unpack_from("<I", data, 4)[0]
    reference = struct.unpack_from("<3d", data, 8)
    records = np.frombuffer(data, dtype=np.dtype([("c", "<f4", (9,)), ("info", "<u4")]), count=count, offset=32)
    eye = np.linalg.inv(w2v)[:3, 3] - np.array(reference)
    corners = records["c"].reshape(-1, 3, 3).astype(np.float64)
    near = (np.linalg.norm(corners - eye, axis=2) < radius).any(axis=1)
    solid = np.isin((records["info"] >> 24) & 15, list(SOLID))
    kept = records[near & solid]

    depth = np.fromfile(trace / "depth.r32", dtype="<f4").reshape(HEIGHT, WIDTH)
    stencil = np.fromfile(trace / "stencil.r8", dtype=np.uint8).reshape(HEIGHT, WIDTH)
    # View depth in meters from the stored reversed depth: clip z = a * z + b, clip w = -z.
    a, b = v2c[2, 2], v2c[2, 3]
    pixels = []
    for y in range(0, HEIGHT, STRIDE):
        for x in range(0, WIDTH, STRIDE):
            s, d = int(stencil[y, x]), float(depth[y, x])
            if (s & 0x84) != 0x80 or s in (129, 136, 137) or not d > 0:
                continue
            if b / (d + a) >= view_m:
                continue
            pixels.append(struct.pack("<HHfB3x", x, y, d, s))

    with open(out, "wb") as f:
        f.write(b"PGF1" + struct.pack("<II", len(pixels), len(kept)))
        f.write(struct.pack("<16f", *world_to_view) + struct.pack("<16f", *view_to_clip) + struct.pack("<3d", *reference))
        f.write(b"".join(pixels))
        f.write(kept.tobytes())
    classes = np.bincount([p[8] for p in pixels], minlength=256)
    print(f"{len(pixels)} grid pixels within {view_m} m ({', '.join(f'{c} of stencil {s}' for s, c in enumerate(classes) if c)}), "
          f"{len(kept)} triangles within {radius} m, {out.stat().st_size} bytes")


if __name__ == "__main__":
    main()
