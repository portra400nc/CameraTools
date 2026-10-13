#!/usr/bin/env python3
"""Predict on the Mac what the gather's sky modes, strengths and levels do to a saved frame, before any Deck picture.

python3 predict_shot.py <trace folder> [scene folder]

The trace folder is what EnhancementsSaveTrace wrote, with the gather's sums: the Deck's own (gather-hit.f32 and
gather-sky.f32) when it gathered, else trace_reference --gather's (reference-gather-hit.f32 and the rest, with its
stand-in light in reference-light.bin). For each variant it computes the irradiance the apply draw
(shaders/apply_gather_ps.hlsl) writes on world pixels with samples, the far terms filtered as it filters them, and the
frame as the game's combine pass would light it with that irradiance:

    hdr' = hdr + albedo x (irradiance' - irradiance-out)

then the game's tone curve at its exposure (trace.tsv's, else the census's 0.985), without bloom. That is exact where the
combine pass is linear in the irradiance and the fog is thin. It writes predict-<variant>.png into the trace folder and
predict.tsv with each variant's mean luminance and its changes against the game's.

A save of T1 has no hdr.r11g11b10, albedo.rgba8 or irradiance-out.r11g11b10. Then the HDR scene stands in from the
scene's view face picture (face-6.png, the back buffer at half size) through the tone curve inverted, which carries the
game's bloom; the albedo is that HDR over the game's light at the pixel, its sun (the stand-in light's color times the
cosine of the shading direction times the game's shadow mask) plus its irradiance, clamped to 0..1; and the irradiance
the frame was lit with is the game's own. It says which stood in.
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scene import write_png  # noqa: E402
from check_trace import r11g11b10, values  # noqa: E402

VARIANTS = (
    # name, sky mode, strength, level, filter
    ("game", None, 0, 0, 0),
    ("traced-t1", "occlusion", 0, None, 2),
    ("bounce", "visibility", 1, None, 2),
    ("bounce-off", "visibility", 0, None, 2),
    ("bounce-additive", "occlusion", 1, None, 2),
    ("bounce-weighted", "weighted", 1, None, 2),
    ("bounce-skymap", "map", 1, None, 2),
    ("bounce-x2", "visibility", 2, None, 2),
    ("bounce-nofilter", "visibility", 1, None, 0),
    ("bounce-level-1", "visibility", 1, 1.0, 2),
)
KERNEL = 2  # the filter's 5x5


def luminance(rgb):
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def light_file(path):
    """trace-light.bin's sky harmonics, sun color and shading direction (core/light.h's layout)."""
    data = open(path, "rb").read()
    count = struct.unpack_from("<I", data, 4)[0]
    at = 8 + count * 8
    side = struct.unpack_from("<I", data, at)[0]
    at += 4 + side * side * 16 + 8
    harmonics = np.array(struct.unpack_from("<27f", data, at)).reshape(9, 3)
    at += 108 + 48 + 12 + 8
    sun = np.array(struct.unpack_from("<3f", data, at))
    shade = np.array(struct.unpack_from("<3f", data, at + 12))
    return harmonics, sun, shade


def sky_irradiance(harmonics, n):
    x, y, z = n[..., 0], n[..., 1], n[..., 2]
    basis = [np.ones_like(x), y, z, x, x * y, y * z, 3 * z * z - 1, x * z, x * x - y * y]
    return sum(b[..., None] * harmonics[i] for i, b in enumerate(basis))


def picture_hdr(stored):
    v = ((stored / 255.0 + 0.055) / 1.055) ** 2.4
    a, b, c = 1.36 - 0.93 * v, 0.047 - 0.56 * v, -0.14 * v
    with np.errstate(invalid="ignore", divide="ignore"):
        x = np.where(a > 1e-6, (-b + np.sqrt(b * b - 4 * a * c)) / (2 * a), 16.0)
    return np.minimum(x, 16.0) / 0.985


def read_png(path):
    import zlib
    data = open(path, "rb").read()
    at, idat, width, height = 8, b"", 0, 0
    while at + 12 <= len(data):
        length, kind = struct.unpack_from(">I4s", data, at)
        if kind == b"IHDR":
            width, height = struct.unpack_from(">II", data, at + 8)
        elif kind == b"IDAT":
            idat += data[at + 8:at + 8 + length]
        at += 12 + length
    raw = np.frombuffer(zlib.decompress(idat), np.uint8).reshape(height, width * 3 + 1)
    return raw[:, 1:].reshape(height, width, 3)


def tone(hdr, exposure):
    x = np.maximum(hdr * exposure, 0)
    curve = np.clip(x * (1.36 * x + 0.047) / (x * (0.93 * x + 0.56) + 0.14), 0, 1)
    return (np.clip(np.maximum(curve ** (1 / 2.4) * 1.055 - 0.055, 0), 0, 1) * 255 + 0.5).astype(np.uint8)


class Filter:
    """shaders/apply_gather_ps.hlsl's edge-aware 5x5 mean: gbuffer.hlsli's blur_weight of view depth and normal."""

    def __init__(self, view_z, normals, world):
        self.z, self.n, self.world = view_z, normals, world

    def __call__(self, image, step):
        h, w = self.z.shape
        total = np.zeros((h, w))
        out = np.zeros(image.shape)
        for dy in range(-KERNEL, KERNEL + 1):
            for dx in range(-KERNEL, KERNEL + 1):
                oy, ox = dy * step, dx * step
                ys, xs = slice(max(0, -oy), h - max(0, oy)), slice(max(0, -ox), w - max(0, ox))
                yq, xq = slice(max(0, oy), h - max(0, -oy)), slice(max(0, ox), w - max(0, -ox))
                z, zq = self.z[ys, xs], self.z[yq, xq]
                depth = np.clip(1 - np.abs(zq - z) / (np.maximum(-z, 0.05) * 0.1), 0, 1)
                facing = (self.n[ys, xs] * self.n[yq, xq]).sum(-1) * 0.5 + 0.5
                weight = depth * facing * facing * self.world[yq, xq] * self.world[ys, xs]
                total[ys, xs] += weight
                out[ys, xs] += image[yq, xq] * (weight[..., None] if image.ndim == 3 else weight)
        total = np.maximum(total, 1e-6)
        return out / (total[..., None] if image.ndim == 3 else total)

    def passes(self, image, count):
        for k in range(count):
            image = self(image, 1 << k)
        return image


def main(folder, scene=None):
    info = values(folder)
    w, h = int(info["width"]), int(info["height"])
    scene = scene or os.path.join(os.path.dirname(os.path.abspath(folder)), info["scene"])
    stood_in = []
    read = lambda name, dtype=np.uint32: np.fromfile(os.path.join(folder, name), dtype)  # noqa: E731
    history = read("history.f32", np.float32).reshape(h, w, 4).astype(np.float64)
    depth = read("depth.r32", np.float32).reshape(h, w)
    stencil = read("stencil.r8", np.uint8).reshape(h, w)
    word = read("normals.r10g10b10a2").reshape(h, w)
    normals = np.stack([(word >> (10 * k)) & 1023 for k in range(3)], -1) / 1023.0 * 2 - 1
    normals /= np.maximum(np.linalg.norm(normals, axis=-1, keepdims=True), 1e-6)
    game = r11g11b10(read("irradiance.r11g11b10").reshape(h, w).astype(np.int64))
    mask = read("mask.r8g8", np.uint8).reshape(h, w, 2)[..., 0] / 255.0
    if os.path.exists(os.path.join(folder, "gather-hit.f32")):
        hit, sky = (read(name, np.float32).reshape(h, w, 4).astype(np.float64) for name in ("gather-hit.f32", "gather-sky.f32"))
        light_path = os.path.join(scene, "trace-light.bin")
    else:
        hit, sky = (read(name, np.float32).reshape(h, w, 4).astype(np.float64) for name in ("reference-gather-hit.f32", "reference-gather-sky.f32"))
        history[..., 0] = read("reference.f32", np.float32).reshape(h, w, 4)[..., 0]
        light_path = os.path.join(folder, "reference-light.bin")
        stood_in.append("the gather's sums are trace_reference's, with its stand-in light")
    harmonics, sun, shade = light_file(light_path)
    world = ((stencil & 0x84) == 0x80) & (depth > 0) & (history[..., 2] > 0)
    samples = np.maximum(history[..., 2], 1)
    if os.path.exists(os.path.join(folder, "hdr.r11g11b10")):
        hdr = r11g11b10(read("hdr.r11g11b10").reshape(h, w).astype(np.int64))
    else:
        picture = read_png(os.path.join(scene, "face-6.png"))
        ys = (np.arange(h) * picture.shape[0] // h)[:, None]
        xs = (np.arange(w) * picture.shape[1] // w)[None, :]
        hdr = picture_hdr(picture[ys, xs].astype(np.float64))
        stood_in.append("the HDR scene is the view face's picture through the tone curve inverted, bloom and all")
    if os.path.exists(os.path.join(folder, "albedo.rgba8")):
        stored = read("albedo.rgba8", np.uint8).reshape(h, w, 4)[..., :3] / 255.0
        albedo = np.where(stored <= 0.04045, stored / 12.92, ((stored + 0.055) / 1.055) ** 2.4)
    else:
        lit = sun * (np.maximum((normals * shade).sum(-1), 0) * mask)[..., None] + game
        albedo = np.clip(hdr / np.maximum(lit, 1e-4), 0, 1)
        stood_in.append("the albedo is the HDR scene over the game's sun and irradiance at the pixel")
    if os.path.exists(os.path.join(folder, "irradiance-out.r11g11b10")):
        written = r11g11b10(read("irradiance-out.r11g11b10").reshape(h, w).astype(np.int64))
    else:
        written = game
        stood_in.append("the frame was lit with the game's own irradiance")
    exposure = float(info.get("exposure", "-")) if info.get("exposure", "-") not in ("-", "") else 0.985
    level = float(info.get("ambient.level", 0.6))
    ao_strength = float(info.get("trace.ao_strength", 1))
    for line in stood_in:
        print(f"STANDIN {line}")

    # The view depth along the camera's axis, as gbuffer.hlsli's view_position gives it: -z = B / (depth + A).
    projection = [float(v) for v in info["view_to_clip"].split(",")]
    view_z = -projection[14] / np.maximum(depth + projection[10], 1e-9)
    smooth = Filter(view_z, normals, world.astype(np.float64))
    occ = history[..., 0] / samples
    vis = sky[..., 3] / samples
    bounce = hit[..., :3] / samples[..., None]
    sky_light = sky[..., :3] / samples[..., None]
    weighted = luminance(sky_light) / np.maximum(luminance(sky_irradiance(harmonics, normals)), 1e-4)

    rows = ["variant\tsky\tstrength\tlevel\tfilter\tmean_luminance\tchange_world\tirradiance_luminance"]
    base = None
    for name, mode, strength, variant_level, filter_passes in VARIANTS:
        lvl = level if variant_level is None else variant_level
        if mode is None:
            irradiance = written
        else:
            occ_f = smooth.passes(occ, filter_passes)
            if mode == "occlusion":
                weight = occ
            elif mode == "visibility":
                weight = occ * smooth.passes(vis, filter_passes) / np.maximum(occ_f, 1e-3) if filter_passes else vis
            elif mode == "weighted":
                weight = occ * smooth.passes(weighted, filter_passes) / np.maximum(occ_f, 1e-3) if filter_passes else weighted
            b = smooth.passes(bounce, filter_passes)
            if mode == "map":
                irradiance = lvl * smooth.passes(sky_light, filter_passes) + strength * b
            else:
                irradiance = lvl * game * (1 + (weight - 1) * ao_strength)[..., None] + strength * b
            irradiance = np.where(world[..., None], irradiance, written)
        shot = hdr + np.where(world[..., None], albedo * (irradiance - written), 0)
        image = tone(shot, exposure)
        write_png(os.path.join(folder, f"predict-{name}.png"), image)
        mean = luminance(image / 255.0).mean()
        world_mean = luminance(shot)[world].mean()
        base = world_mean if base is None else base
        rows.append(f"{name}\t{mode or '-'}\t{strength}\t{lvl:g}\t{filter_passes}\t{mean:.4f}\t{world_mean / base - 1:+.4f}\t{luminance(irradiance)[world].mean():.4f}")
        print(f"INFO {name}: the picture's mean {mean:.4f}, the world's HDR {world_mean / base - 1:+.1%} against the game's, its irradiance's mean "
              f"luminance {luminance(irradiance)[world].mean():.4f}")
    open(os.path.join(folder, "predict.tsv"), "w").write("\n".join(rows) + "\n")
    print(f"{folder}: {len(VARIANTS)} variants predicted into predict-*.png and predict.tsv")
    return 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:3]))
