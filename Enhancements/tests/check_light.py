#!/usr/bin/env python3
"""Report on an ENHANCEMENTS_SCENE_LIGHT capture's light, after bake_light has checked it. numpy only.

python3 check_light.py <scene folder> [trace folder]

3. With a trace folder whose save gathered: the gather's hits by where their triangle's albedo came from, from
   trace.tsv's counters. Direct plus covered should be 55% or more.
4. The sky map: real texels at least 25% of the map, and check-light-sky.png beside the scene's files: the map, real
   texels as they are and filled ones dimmed by half, beside the six cube faces' pictures. With a trace folder, the game's
   irradiance over the map's own on open up-facing world pixels (normal y above 0.9, stencil 128), as information: near 1
   means the game's ambient light is its sky's.
5. With a trace folder holding the view's HDR scene and albedo: the sun's color S' that fits hdr = albedo x (S' x cosine
   x mask + irradiance) on stencil 128 pixels the sun lights, against the combine constants' S, as information. Near 1
   says the irradiance buffer and the sun's color are in the unit the gather assumes.
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scene import write_png  # noqa: E402
from check_trace import r11g11b10, report, values  # noqa: E402
import check_trace  # noqa: E402

SIDE = 128


def light(path):
    data = open(path, "rb").read()
    count = struct.unpack_from("<I", data, 4)[0]
    at = 8 + count * 8
    side = struct.unpack_from("<I", data, at)[0]
    texels = np.frombuffer(data, np.float32, side * side * 4, at + 4).reshape(side, side, 4)
    at += 4 + side * side * 16
    real, fallback = struct.unpack_from("<2I", data, at)
    at += 8
    harmonics = np.array(struct.unpack_from("<27f", data, at)).reshape(9, 3)
    at += 108 + 48 + 12 + 8
    sun = np.array(struct.unpack_from("<3f", data, at))
    shade = np.array(struct.unpack_from("<3f", data, at + 12))
    return texels, real, fallback, harmonics, sun, shade


def main(scene, trace=None):
    texels, real, fallback, harmonics, sun, shade = light(os.path.join(scene, "trace-light.bin"))
    info = values(trace) if trace else {}
    if trace and info.get("gathering") == "1":
        hits = [int(info.get(f"gather_hits_{name}", 0)) for name in ("direct", "covered", "material", "class")]
        total = max(sum(hits), 1)
        share = (hits[0] + hits[1]) / total
        report("PASS" if share >= 0.55 else "FAIL", f"3 the gather's hits by albedo source: direct {hits[0] / total:.3f}, covered {hits[1] / total:.3f}, "
                                                    f"material {hits[2] / total:.3f}, class {hits[3] / total:.3f}; direct and covered {share:.3f} (need 0.55)")
    else:
        report("INFO", "3 no gathering save: no sources by gather hit")

    report("PASS" if real >= 0.25 * SIDE * SIDE and not fallback else "FAIL",
           f"4 the sky map has {real} real texels of {SIDE * SIDE} ({real / SIDE / SIDE:.3f}; need 0.25){', and fell back' if fallback else ''}")
    rgb = texels[..., :3] * np.where(texels[..., 3:] > 0, 1.0, 0.5)
    tile = (np.clip(rgb / (1 + rgb), 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)
    tile = np.repeat(np.repeat(tile, 2, 0), 2, 1)
    row = [tile]
    for face in range(6):
        path = os.path.join(scene, f"face-{face}.png")
        if os.path.exists(path):
            from predict_shot import read_png
            picture = read_png(path)
            ys = np.arange(256) * picture.shape[0] // 256
            xs = np.arange(256 * picture.shape[1] // picture.shape[0]) * picture.shape[1] // (256 * picture.shape[1] // picture.shape[0])
            row.append(picture[ys][:, xs])
    write_png(os.path.join(scene, "check-light-sky.png"), np.concatenate(row, axis=1))

    if not trace:
        return 0 if check_trace.failures == 0 else 1
    w, h = int(info["width"]), int(info["height"])
    read = lambda name, dtype=np.uint32: np.fromfile(os.path.join(trace, name), dtype)  # noqa: E731
    stencil = read("stencil.r8", np.uint8).reshape(h, w)
    depth = read("depth.r32", np.float32).reshape(h, w)
    word = read("normals.r10g10b10a2").reshape(h, w)
    n = np.stack([(word >> (10 * k)) & 1023 for k in range(3)], -1) / 1023.0 * 2 - 1
    n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
    irradiance = r11g11b10(read("irradiance.r11g11b10").reshape(h, w).astype(np.int64))
    x, y, z = n[..., 0], n[..., 1], n[..., 2]
    basis = [np.ones_like(x), y, z, x, x * y, y * z, 3 * z * z - 1, x * z, x * x - y * y]
    sky_light = sum(b[..., None] * harmonics[i] for i, b in enumerate(basis))
    lum = lambda c: c[..., 0] * 0.2126 + c[..., 1] * 0.7152 + c[..., 2] * 0.0722  # noqa: E731
    up = (stencil == 128) & (depth > 0) & (y > 0.9)
    if up.any():
        report("INFO", f"4 the game's irradiance over the sky map's on {up.sum()} up-facing world pixels: {np.median(lum(irradiance[up]) / np.maximum(lum(sky_light[up]), 1e-4)):.3f} "
                       f"(median; near 1 means the game's ambient light is its sky's)")
    if not (os.path.exists(os.path.join(trace, "hdr.r11g11b10")) and os.path.exists(os.path.join(trace, "albedo.rgba8"))):
        report("INFO", "5 the save holds no hdr.r11g11b10 or albedo.rgba8: no fit of the sun's color")
        return 0 if check_trace.failures == 0 else 1
    hdr = r11g11b10(read("hdr.r11g11b10").reshape(h, w).astype(np.int64))
    stored = read("albedo.rgba8", np.uint8).reshape(h, w, 4)[..., :3] / 255.0
    albedo = np.where(stored <= 0.04045, stored / 12.92, ((stored + 0.055) / 1.055) ** 2.4)
    mask = read("mask.r8g8", np.uint8).reshape(h, w, 2)[..., 0] / 255.0 if os.path.exists(os.path.join(trace, "mask.r8g8")) else np.ones((h, w))
    cosine = np.maximum((n * shade).sum(-1), 0) * mask
    lit = (stencil == 128) & (depth > 0) & (cosine > 0.2) & (albedo.min(-1) > 0.02)
    if lit.sum() < 100:
        report("INFO", "5 too few sunlit stencil 128 pixels to fit the sun's color")
        return 0 if check_trace.failures == 0 else 1
    a, c = albedo[lit], cosine[lit][:, None]
    fitted = ((a * c) * (hdr[lit] - a * irradiance[lit])).sum(0) / ((a * c) ** 2).sum(0)
    report("INFO", f"5 the sun's color fitted to {lit.sum()} sunlit pixels of stencil 128: S' {np.round(fitted, 3)} against the combine constants' S "
                   f"{np.round(sun, 3)}, S'/S {np.round(fitted / np.maximum(sun, 1e-6), 3)}")
    return 0 if check_trace.failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:3]))
