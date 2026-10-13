#!/usr/bin/env python3
"""Checks an Enhancements scene capture folder on the Mac: python3 check_scene.py <scene folder>. Needs only numpy.

1. Replay fidelity: scene.tsv's depth check for stencil 128 has matched / (tested - behind) >= 0.98 on every face, every
   direct draw in draws.tsv wrote the triangles it should, no replayed draw left a corner of its reservation unwritten,
   and no draw was skipped for a reason the add-on does not name.
2. World placement: each face's depth, rendered at quarter size from all faces' meshes through that face's camera, agrees
   with the face's own depth buffer within 2% of the distance on at least 95% of the pixels with depth. The face opposite
   the view is placed by its own buffer too, so this proves the geometry behind the camera is in the right place.
   check-face-N.png shows the game's depth, the meshes' depth and where they disagree.
3. Coverage: of 20,000 directions over the sphere, the share in which the meshes hit something, all faces against the
   view alone. Reported, not judged.
4. Consistency: stencil 128 corners one face sees that another face also sees, by that face's depth buffer, have a corner
   of the other face's mesh within 5 mm at the 95th percentile.
5. Winding: the share of the view's main-pass triangles that face the camera. Reported.
6. World frame: b1 row 5 against the pushed camera's position for each face. Reported.

Prints one PASS, FAIL or INFO line per finding as it goes and exits 1 when a check fails. The meshes are drawn with
vertices projected once per camera and triangles outside its frustum dropped, so seven faces of a few million triangles
each take minutes, not tens of minutes.
"""

import csv
import math
import os
import struct
import sys
import zlib

import numpy as np

KNOWN_SKIPS = {"-", "no-bytecode", "gs-bound", "no-position", "game-so", "query-pool", "gs-create-failed", "topology", "buffer-full"}
failed = False


def report(kind, text):
    global failed
    failed |= kind == "FAIL"
    print(f"{kind} {text}", flush=True)


def rows(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f, delimiter="\t"))


def numbers(cell, count):
    values = np.array([float(v) for v in cell.split(",")], dtype=np.float64)
    assert len(values) == count, cell
    return values


def read_obj(path):
    """Vertices (N, 3), triangles (M, 3) from 0, and each triangle's class and kind from its usemtl and g lines."""
    vertices, faces, runs = [], [], []
    kind = "main"
    with open(path, "rb") as f:
        for line in f:
            head = line[:2]
            if head == b"v ":
                vertices.append(line[2:])
            elif head == b"f ":
                faces.append(line[2:])
            elif head == b"g ":
                kind = line[2:].split(b"_")[1].decode()
            elif line.startswith(b"usemtl "):
                runs.append((len(faces), line[7:].strip().decode(), kind))
    v = np.array(b" ".join(vertices).split(), dtype=np.float64).reshape(-1, 3)
    t = np.array(b" ".join(faces).split(), dtype=np.int64).reshape(-1, 3) - 1
    classes = np.empty(len(t), dtype=object)
    kinds = np.empty(len(t), dtype=object)
    for i, (start, name, run_kind) in enumerate(runs):
        end = runs[i + 1][0] if i + 1 < len(runs) else len(t)
        classes[start:end] = name
        kinds[start:end] = run_kind
    return v, t, classes, kinds


def read_ply(path):
    data = open(path, "rb").read()
    end = data.index(b"end_header\n") + len(b"end_header\n")
    header = data[:end].decode().split("\n")
    count = {line.split()[1]: int(line.split()[2]) for line in header if line.startswith("element")}
    v = np.frombuffer(data, dtype="<f4", count=count["vertex"] * 3, offset=end).reshape(-1, 3).astype(np.float64)
    faces = np.frombuffer(data, dtype=np.dtype([("n", "u1"), ("i", "<i4", (3,))]), count=count["face"], offset=end + count["vertex"] * 12)
    t = faces["i"].astype(np.int64)
    unknown = np.full(len(t), None, dtype=object)
    return v, t, unknown, unknown


class Face:
    def __init__(self, folder, scene, camera):
        self.index = int(scene["face"])
        self.name = scene["name"]
        self.size = (int(scene["width"]), int(scene["height"]))
        self.origin = numbers(scene["origin"], 3)
        self.position = numbers(scene["position"], 3)
        self.forward = numbers(scene["forward"], 3)
        self.scene = scene
        self.clip_to_world = numbers(camera["clip_to_world"], 16).reshape(4, 4)
        self.world_to_clip = np.linalg.inv(self.clip_to_world)
        self.camera = camera
        stem = f"face-{self.index}-{self.name}"
        obj, ply = os.path.join(folder, stem + ".obj"), os.path.join(folder, stem + ".ply")
        v, self.triangles, self.classes, self.kinds = read_obj(obj) if os.path.exists(obj) else read_ply(ply)
        # Undo the mesh convention: positions were written relative to the origin with z negated.
        self.vertices = np.c_[v[:, 0], v[:, 1], -v[:, 2]] + self.origin
        width, height = self.size
        raw = np.fromfile(os.path.join(folder, f"face-{self.index}-depth.r32"), dtype="<f4")
        self.depth = raw.reshape(height, width) if raw.size == width * height else None
        self.meters = self.buffer_meters() if self.depth is not None else None

    def buffer_meters(self):
        """The depth buffer in meters along the camera's forward axis, NaN where it holds nothing (reversed depth 0)."""
        width, height = self.size
        x = (np.arange(width) + 0.5) / width * 2 - 1
        y = 1 - (np.arange(height) + 0.5) / height * 2
        xx, yy = np.meshgrid(x, y)
        clip = np.stack([xx, yy, self.depth.astype(np.float64), np.ones_like(xx)], axis=-1)
        world = clip @ self.clip_to_world.T
        world = world[..., :3] / world[..., 3:4]
        meters = (world - self.position) @ self.forward
        meters[self.depth <= 0] = np.nan
        return meters

    def forward_scale(self):
        """Meters along the forward axis per unit of this camera's clip w."""
        ahead = np.r_[self.position + self.forward, 1] @ self.world_to_clip.T
        return 1 / ahead[3]

    def project(self, points):
        """Pixel column, row (floats) and view depth of world points as this face's camera sees them, and whether in view."""
        h = np.c_[points, np.ones(len(points))] @ self.world_to_clip.T
        w = h[:, 3]
        with np.errstate(divide="ignore", invalid="ignore"):
            column = (h[:, 0] / w * 0.5 + 0.5) * self.size[0]
            row = (0.5 - 0.5 * h[:, 1] / w) * self.size[1]
        inside = (w > 1e-3) & (column >= 0) & (column < self.size[0]) & (row >= 0) & (row < self.size[1])
        return column, row, (points - self.position) @ self.forward, inside


def splat(world_to_clip, forward_scale, size, faces):
    """A z-buffer of view depth at size, of each mesh's triangles sampled on a grid finer than a pixel. Vertices are
    projected once, and triangles wholly outside one side of the frustum are dropped. A triangle in front of the camera
    whose corners lie within a screen's width of the picture is sampled in screen space, in rows along its longest edge
    0.36 pixels apart, with its depth interpolated perspective-correct; so a long thin triangle costs about its area, not
    its length squared. Any other, one that crosses the camera plane or reaches far off the picture, is sampled in clip
    space, which is exact for points on it, on a square grid as fine as its size at its nearest corner. Depth is clip w
    times forward_scale."""
    width, height = size
    zbuffer = np.full(height * width, np.inf)
    # Pixels per meter at 1 m of depth: the projection's vertical scale over the half height.
    per_meter = np.linalg.norm(world_to_clip[1, :3]) / np.linalg.norm(world_to_clip[3, :3]) * height / 2
    density = 2.8
    to_bucket = lambda n: np.left_shift(1, np.ceil(np.log2(np.clip(n, 1, 256))).astype(np.int64))

    def draw(points, inverse_w):
        """points: (n, 2) pixel columns and rows; inverse_w: 1 / clip w of each."""
        ok = (inverse_w > 0) & (points[:, 0] >= 0) & (points[:, 0] < width) & (points[:, 1] >= 0) & (points[:, 1] < height)
        index = points[ok, 1].astype(np.int64) * width + points[ok, 0].astype(np.int64)
        np.minimum.at(zbuffer, index, forward_scale / inverse_w[ok])

    def grids(keys):
        for key in np.unique(keys):
            b_along, b_across = key // 512, key % 512
            i, j = np.meshgrid(np.arange(b_along + 1), np.arange(b_across + 1))
            keep = i * b_across + j * b_along <= b_along * b_across
            # Each row's end on the third edge too, so all three edges are sampled as finely as the rows are spaced.
            rows = np.arange(b_across + 1) / b_across
            u, k = np.r_[i[keep] / b_along, 1 - rows], np.r_[j[keep] / b_across, rows]
            yield keys == key, u, k, max(1, 4_000_000 // len(u))

    for face in faces:
        v, t = face.vertices, face.triangles
        if len(t) == 0:
            continue
        h = np.c_[v, np.ones(len(v))] @ world_to_clip.T
        x, y, w = h[t, 0], h[t, 1], h[t, 3]
        outside = (np.all(x > w, axis=1) | np.all(x < -w, axis=1) | np.all(y > w, axis=1) | np.all(y < -w, axis=1)
                   | np.all(w <= 1e-3, axis=1))
        t = t[~outside]
        if len(t) == 0:
            continue
        ht = h[t]
        safe_w = np.where(ht[..., 3] > 1e-3, ht[..., 3], 1)
        screen = np.stack([(ht[..., 0] / safe_w * 0.5 + 0.5) * width, (0.5 - 0.5 * ht[..., 1] / safe_w) * height], axis=-1)
        near_picture = np.all((screen[..., 0] > -width) & (screen[..., 0] < 2 * width) & (screen[..., 1] > -height) & (screen[..., 1] < 2 * height), axis=1)
        flat = np.all(ht[..., 3] > 1e-3, axis=1) & near_picture

        # In screen space: corner a and b end the longest edge, c is opposite.
        p, inverse_w = screen[flat], 1 / ht[flat][..., 3]
        edges = np.linalg.norm(np.roll(p, -1, axis=1) - p, axis=2)
        order = (np.argmax(edges, axis=1)[:, None] + np.arange(3)[None, :]) % 3
        p = np.take_along_axis(p, order[..., None], axis=1)
        inverse_w = np.take_along_axis(inverse_w, order, axis=1)
        base = np.max(edges, axis=1)
        area2 = np.abs((p[:, 1, 0] - p[:, 0, 0]) * (p[:, 2, 1] - p[:, 0, 1]) - (p[:, 2, 0] - p[:, 0, 0]) * (p[:, 1, 1] - p[:, 0, 1]))
        across = area2 / np.maximum(base, 1e-9)
        keys = to_bucket(np.ceil(base * density)) * 512 + to_bucket(np.ceil(across * density))
        for chosen, u, k, step in grids(keys):
            pc, wc = p[chosen], inverse_w[chosen]
            for start in range(0, len(pc), step):
                a, iw = pc[start:start + step], wc[start:start + step]
                points = a[:, 0:1] + u[None, :, None] * (a[:, 1:2] - a[:, 0:1]) + k[None, :, None] * (a[:, 2:3] - a[:, 0:1])
                inverse = iw[:, 0:1] + u[None, :] * (iw[:, 1:2] - iw[:, 0:1]) + k[None, :] * (iw[:, 2:3] - iw[:, 0:1])
                draw(points.reshape(-1, 2), inverse.reshape(-1))

        # In clip space: as fine as the triangle's size at its nearest corner in front.
        rest, ht = t[~flat], ht[~flat]
        if len(rest) == 0:
            continue
        depth = ht[..., 3] * forward_scale
        near = np.maximum(np.min(np.where(depth > 0, depth, np.inf), axis=1), 0.05)
        corners = v[rest]
        edge = np.max(np.linalg.norm(corners - np.roll(corners, 1, axis=1), axis=2), axis=1)
        n = to_bucket(np.ceil(edge * per_meter / near * density))
        for chosen, u, k, step in grids(n * 512 + n):
            hc = ht[chosen]
            for start in range(0, len(hc), step):
                tri = hc[start:start + step]
                samples = (tri[:, 0:1] + u[None, :, None] * (tri[:, 1:2] - tri[:, 0:1]) + k[None, :, None] * (tri[:, 2:3] - tri[:, 0:1])).reshape(-1, 4)
                sw = samples[:, 3]
                ok = sw > 1e-3
                points = np.c_[(samples[ok, 0] / sw[ok] * 0.5 + 0.5) * width, (0.5 - 0.5 * samples[ok, 1] / sw[ok]) * height]
                draw(points, 1 / sw[ok])
    return zbuffer.reshape(height, width)


def write_png(path, rgb):
    height, width, _ = rgb.shape
    raw = b"".join(b"\0" + rgb[y].tobytes() for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def shade(meters):
    gray = np.where(np.isfinite(meters), 255 * np.exp(-np.nan_to_num(meters, nan=0, posinf=0) / 40), 0)
    return np.repeat(gray[..., None], 3, axis=2).astype(np.uint8)


def check_replay(faces, draws):
    for face in faces:
        s = face.scene
        tested, behind, matched = int(s["s128_tested"]), int(s["s128_behind"]), int(s["s128_matched"])
        seen = tested - behind
        share = matched / seen if seen else float("nan")
        if seen == 0:
            report("INFO", f"1 {face.name}: no stencil 128 corner was tested")
        else:
            report("PASS" if share >= 0.98 else "FAIL", f"1 {face.name}: stencil 128 matched {matched} of {seen} unoccluded corners ({share:.4f}, need 0.98)")
    wrong = [d for d in draws if d["expected"] != "-" and d["skip"] == "-" and d["expected"] != d["written"]]
    report("PASS" if not wrong else "FAIL", f"1 every direct draw wrote its expected triangles ({len(wrong)} did not"
           + (f", first face {wrong[0]['face']} draw {wrong[0]['draw']}: {wrong[0]['written']} of {wrong[0]['expected']})" if wrong else ")"))
    # Folders from before build 199 have no unwritten column; their draws streamed one corner a triangle.
    if draws and "unwritten" in draws[0]:
        short = [d for d in draws if d["skip"] == "-" and int(d["unwritten"]) > 0]
        report("PASS" if not short else "FAIL", f"1 every replayed draw wrote every corner of its reservation ({len(short)} did not"
               + (f", first face {short[0]['face']} draw {short[0]['draw']}: {short[0]['unwritten']} of {short[0]['corners']} unwritten)" if short else ")"))
    else:
        report("INFO", "1 draws.tsv has no unwritten column: a capture from before the stream-output fix")
    unknown = {d["skip"] for d in draws} - KNOWN_SKIPS
    report("PASS" if not unknown else "FAIL", f"1 no draw skipped for an unknown reason{': ' + ', '.join(sorted(unknown)) if unknown else ''}")
    skips = {}
    for d in draws:
        if d["skip"] != "-":
            skips[d["skip"]] = skips.get(d["skip"], 0) + 1
    if skips:
        report("INFO", "1 skipped draws: " + ", ".join(f"{k} {v}" for k, v in sorted(skips.items())))


def check_placement(folder, faces):
    for face in faces:
        if face.meters is None:
            report("FAIL", f"2 {face.name}: no depth buffer of {face.size[0]}x{face.size[1]}")
            continue
        width, height = face.size[0] // 4, face.size[1] // 4
        mesh = splat(face.world_to_clip, face.forward_scale(), (width, height), faces)
        # The game's depth at quarter size: the nearest of each 4x4 block, like the z-buffer's nearest sample.
        blocks = face.meters[:height * 4, :width * 4].reshape(height, 4, width, 4)
        with np.errstate(all="ignore"):
            game = np.nanmin(blocks, axis=(1, 3))
        has = np.isfinite(game)
        agree = has & (np.abs(mesh - game) <= 0.02 * game)
        share = agree.sum() / max(has.sum(), 1)
        report("PASS" if share >= 0.95 else "FAIL", f"2 {face.name}: the meshes' depth agrees with the game's within 2% on {share:.4f} of {has.sum()} pixels with depth (need 0.95)")
        difference = np.zeros((height, width, 3), dtype=np.uint8)
        difference[has & ~agree] = (255, 0, 0)
        difference[agree] = (0, 160, 0)
        difference[~has & np.isfinite(mesh)] = (0, 0, 255)
        write_png(os.path.join(folder, f"check-face-{face.index}.png"), np.concatenate([shade(game), shade(mesh), difference], axis=1))


def cube_cameras(position):
    """Six square 90-degree cameras at position, as (world_to_clip, forward), in the clip convention the faces use."""
    out = []
    for forward, up in [((1, 0, 0), (0, 1, 0)), ((-1, 0, 0), (0, 1, 0)), ((0, 1, 0), (0, 0, -1)), ((0, -1, 0), (0, 0, 1)), ((0, 0, 1), (0, 1, 0)),
                        ((0, 0, -1), (0, 1, 0))]:
        f, u = np.array(forward, float), np.array(up, float)
        r = np.cross(u, f)
        view = np.eye(4)
        view[0, :3], view[1, :3], view[2, :3] = r, u, -f
        view[:3, 3] = -view[:3, :3] @ position
        projection = np.array([[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 0, 0.1], [0, 0, -1, 0]], float)
        out.append((projection @ view, f))
    return out


def check_coverage(faces, view, size=96, rays=20000, reach=5000.0):
    print("3 drawing the meshes into six cube faces, all faces and then the view alone", flush=True)
    position = faces[0].position
    golden = math.pi * (3 - math.sqrt(5))
    k = np.arange(rays)
    y = 1 - 2 * (k + 0.5) / rays
    directions = np.c_[np.cos(golden * k) * np.sqrt(1 - y * y), y, np.sin(golden * k) * np.sqrt(1 - y * y)]

    def hits(meshes):
        hit = np.zeros(rays, dtype=bool)
        for world_to_clip, forward in cube_cameras(position):
            zbuffer = splat(world_to_clip, 1.0, (size, size), meshes)
            h = np.c_[position + directions, np.ones(rays)] @ world_to_clip.T
            w = h[:, 3]
            with np.errstate(divide="ignore", invalid="ignore"):
                column = (h[:, 0] / w * 0.5 + 0.5) * size
                row = (0.5 - 0.5 * h[:, 1] / w) * size
            ok = (w > 1e-3) & (column >= 0) & (column < size) & (row >= 0) & (row < size)
            index = (row[ok].astype(np.int64), column[ok].astype(np.int64))
            hit[np.flatnonzero(ok)] |= zbuffer[index] < reach
        return hit.mean()

    report("INFO", f"3 coverage of {rays} directions: all faces {hits(faces):.4f}, the view alone {hits([view]) if view else float('nan'):.4f}")


def nearest_distances(queries, points, cell=0.02, per_cell=4):
    """The distance from each query to its nearest point, searched in the 27 cells of a grid around it; 3 cells' width
    when none is there."""
    if len(points) == 0:
        return np.full(len(queries), 3 * cell)
    keys = np.floor(points / cell).astype(np.int64)
    pack = lambda k: ((k[:, 0] + (1 << 20)) << 42) | ((k[:, 1] + (1 << 20)) << 21) | (k[:, 2] + (1 << 20))
    packed = pack(keys)
    order = np.argsort(packed)
    packed, sorted_points = packed[order], points[order]
    best = np.full(len(queries), 3 * cell)
    base = np.floor(queries / cell).astype(np.int64)
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dz in (-1, 0, 1):
                q = pack(base + np.array([dx, dy, dz]))
                lo, hi = np.searchsorted(packed, q, "left"), np.searchsorted(packed, q, "right")
                for k in range(per_cell):
                    has = lo + k < hi
                    idx = np.minimum(lo + k, len(packed) - 1)
                    d = np.linalg.norm(sorted_points[idx] - queries, axis=1)
                    best = np.where(has, np.minimum(best, d), best)
    return best


def check_consistency(faces, sample=5000):
    rng = np.random.default_rng(1)
    distances = []
    for a in faces:
        print(f"4 {a.name}: its stencil 128 corners against the other faces", flush=True)
        corners_a = np.unique(a.triangles[a.classes == "s128"].ravel())
        if len(corners_a) == 0:
            continue
        points_a = a.vertices[corners_a]
        for b in faces:
            if b is a or b.meters is None:
                continue
            column, row, depth, inside = b.project(points_a)
            seen = np.flatnonzero(inside)
            if len(seen) == 0:
                continue
            buffer = b.meters[row[seen].astype(np.int64), column[seen].astype(np.int64)]
            seen = seen[np.abs(depth[seen] - buffer) <= np.maximum(0.05, 0.01 * buffer)]
            if len(seen) > sample:
                seen = rng.choice(seen, sample, replace=False)
            corners_b = np.unique(b.triangles[b.classes == "s128"].ravel())
            distances.append(nearest_distances(points_a[seen], b.vertices[corners_b]))
    if not distances:
        report("INFO", "4 no stencil 128 corner is seen by two faces")
        return
    d = np.concatenate(distances)
    p95 = np.percentile(d, 95)
    report("PASS" if p95 <= 0.005 else "FAIL", f"4 stencil 128 corners seen by two faces: the other face's nearest corner within {p95 * 1000:.2f} mm at the 95th percentile"
           f" of {len(d)}, {np.mean(d <= 0.005):.4f} within 5 mm (need 5 mm)")


def check_winding(view):
    if view is None or len(view.triangles) == 0:
        report("INFO", "5 no view mesh")
        return
    main = view.kinds != "pre" if view.kinds[0] is not None else np.ones(len(view.triangles), dtype=bool)
    tri = view.vertices[view.triangles[main]]
    # The reader undid the mirror, so this is Unity's left-handed space, where the file's corner order is a, c, b and a
    # front face's cross(b - a, c - a) points at the viewer.
    a, b, c = tri[:, 0], tri[:, 2], tri[:, 1]
    normal = np.cross(b - a, c - a)
    facing = np.einsum("ij,ij->i", normal, view.position - a) > 0
    report("INFO", f"5 {facing.mean():.4f} of the view's {len(tri)} main-pass triangles face the camera")


def check_frame(faces):
    for face in faces:
        b1 = face.camera.get("b1_5", "-")
        pushed = face.camera.get("world_to_view", "-")
        if b1 == "-" or pushed == "-":
            report("INFO", f"6 {face.name}: {'no b1 row 5' if b1 == '-' else 'no pushed camera'}")
            continue
        view = numbers(pushed, 16).reshape(4, 4)
        position = -view[:3, :3].T @ view[:3, 3]
        offset = numbers(b1, 4)[:3] - position
        report("INFO", f"6 {face.name}: b1 row 5 minus the pushed camera's position = ({offset[0]:.4f}, {offset[1]:.4f}, {offset[2]:.4f})")


def main(folder):
    scene = rows(os.path.join(folder, "scene.tsv"))
    cameras = {row["face"]: row for row in rows(os.path.join(folder, "camera.tsv"))}
    draws = rows(os.path.join(folder, "draws.tsv"))
    faces = []
    for row in scene:
        print(f"reading face {row['face']} ({row['name']})", flush=True)
        faces.append(Face(folder, row, cameras[row["face"]]))
    view = next((f for f in faces if f.name == "view"), None)
    report("INFO", f"{folder}: {len(faces)} faces, {sum(len(f.triangles) for f in faces)} triangles")
    if os.path.exists(os.path.join(folder, "stopped.txt")):
        report("INFO", "stopped: " + open(os.path.join(folder, "stopped.txt")).read().strip())
    check_replay(faces, draws)
    check_placement(folder, faces)
    check_coverage(faces, view)
    check_consistency(faces)
    check_winding(view)
    check_frame(faces)
    print(f"{folder}: {'some checks failed' if failed else 'every check passed'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
