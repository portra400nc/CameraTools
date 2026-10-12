#!/usr/bin/env python3
"""Check a frame PhotorealSaveTrace saved against the Mac's reference tracer. numpy only.

python3 check_trace.py <trace folder>

Run trace_reference on the folder first: it writes reference.f32, the same pixels traced on the Mac with the compute
shader's own code, and reference-squares.f32, each pixel's samples squared and summed. The GPU's float rounding turns some
of the Deck's rays a little off the Mac's, so the two are independent estimates of each pixel's occlusion and sun, and the
checks are statistical. On every world pixel with samples:

1. The samples: the Deck's counts and the reference's are the same.
2. Each pixel's averages differ by at most 5 standard errors on all but 0.1% of the pixels, with each pixel's variance
   taken as p(1 - p) of the pooled average p of both, which bounds the variance of samples between 0 and 1.
3. Each 32x32 tile's mean difference is within 5 standard errors plus 0.002, with each pixel's variance from the
   reference's own samples, at least p(1 - p) / (2n + 1).
4. The mean difference over the whole frame is within 5 standard errors plus 0.0005.

A logic error that changes what a pixel's rays meet moves whole tiles or many pixels by far more than their noise, which
2 to 4 catch; the noise of independent rays does not. It writes check-trace.png: the Deck's occlusion and sun averages, the reference's, and their difference times 10, a row
each. When the folder holds the game's shadow mask and the shadow-mask constants, it reports as information how often the
game's mask and the traced sun disagree within the game's shadow distance and cascades: game shadowed and traced lit
means a caster the scene lacks. It also reports how the traced occlusion follows the game's diffuse irradiance.
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scene import write_png  # noqa: E402

failures = 0


def report(kind, text):
    global failures
    if kind == "FAIL":
        failures += 1
    print(f"{kind} {text}", flush=True)


def values(folder):
    out = {}
    with open(os.path.join(folder, "trace.tsv")) as f:
        for line in f:
            key, _, value = line.rstrip("\n").partition("\t")
            out[key] = value
    return out


def averages(history, which):
    """The average of a sum (0 occlusion, 1 sun) over its count, NaN where there is none."""
    count = history[..., 2 + which]
    with np.errstate(invalid="ignore", divide="ignore"):
        return np.where(count > 0, history[..., which] / np.maximum(count, 1), np.nan)


def tile_sums(values, valid, size):
    h, w = values.shape
    ph, pw = (h + size - 1) // size * size, (w + size - 1) // size * size
    padded = np.zeros((ph, pw))
    padded[:h, :w] = np.where(valid, values, 0)
    return padded.reshape(ph // size, size, pw // size, size).sum(axis=(1, 3))


def sampling_checks(name, deck, mine, squares, valid):
    """Checks 2 to 4 for one kind of ray: deck and mine are (sum, count) images, squares the reference's squared samples."""
    n_deck, n_mine = np.maximum(deck[1], 1), np.maximum(mine[1], 1)
    mean_mine = mine[0] / n_mine
    difference = deck[0] / n_deck - mean_mine
    pooled = (deck[0] + mine[0] + 0.5) / (deck[1] + mine[1] + 1)
    bound = pooled * (1 - pooled)
    z = difference / np.sqrt(bound * (1 / n_deck + 1 / n_mine))
    outliers = (np.abs(z[valid]) > 5).mean()
    report("PASS" if outliers <= 0.001 else "FAIL", f"2 {name}: {outliers:.5f} of {valid.sum()} pixels differ by more than 5 standard errors "
                                                    f"(need at most 0.001; largest {np.abs(z[valid]).max():.1f})")
    variance = np.maximum(squares / n_mine - mean_mine ** 2, bound / (2 * n_mine + 1))
    error2 = variance * (1 / n_deck + 1 / n_mine)
    counts = tile_sums(np.ones_like(difference), valid, 32)
    full = counts >= 256
    tile_mean = tile_sums(difference, valid, 32)[full] / counts[full]
    tile_error = np.sqrt(tile_sums(error2, valid, 32)[full]) / counts[full]
    excess = np.abs(tile_mean) - (5 * tile_error + 0.002)
    worst = int(np.argmax(excess)) if len(excess) else 0
    report("PASS" if not len(excess) or excess.max() <= 0 else "FAIL",
           f"3 {name}: every 32x32 tile's mean difference is within 5 standard errors plus 0.002 ({full.sum()} tiles; the one nearest to or past its bound: "
           f"{tile_mean[worst] if len(excess) else 0:+.5f} against {5 * tile_error[worst] + 0.002 if len(excess) else 0:.5f})")
    mean = difference[valid].mean()
    error = np.sqrt(error2[valid].sum()) / valid.sum()
    report("PASS" if abs(mean) <= 5 * error + 0.0005 else "FAIL",
           f"4 {name}: the frame's mean difference {mean:+.6f} is within 5 standard errors plus 0.0005 ({5 * error + 0.0005:.6f})")
    return np.abs(difference)


def gray(image):
    v = np.nan_to_num(image, nan=0.0)
    return np.repeat((np.clip(v, 0, 1) * 255).astype(np.uint8)[..., None], 3, axis=2)


def red(difference):
    out = np.zeros(difference.shape + (3,), np.uint8)
    out[..., 0] = (np.clip(np.nan_to_num(difference) * 10, 0, 1) * 255).astype(np.uint8)
    return out


def r11g11b10(words):
    """R11G11B10_FLOAT to linear RGB."""
    def small_float(bits, mantissa_bits):
        e = (bits >> mantissa_bits) & 31
        m = bits & ((1 << mantissa_bits) - 1)
        scale = float(1 << mantissa_bits)
        return np.where(e == 0, m / scale * 2.0 ** -14, (1 + m / scale) * 2.0 ** (e.astype(np.float64) - 15))
    return np.stack([small_float(words & 0x7FF, 6), small_float((words >> 11) & 0x7FF, 6), small_float((words >> 22) & 0x3FF, 5)], axis=-1)


def scene_positions(folder, depth):
    """Each pixel's surface in the scene's frame and its distance from the eye, as trace.hlsli reconstructs them."""
    raw = open(os.path.join(folder, "constants.bin"), "rb").read()
    rows = np.array(struct.unpack_from("<16f", raw, 0)).reshape(4, 4)
    eye = np.array(struct.unpack_from("<3f", raw, 64))
    width, height, flip, _ = struct.unpack_from("<4I", raw, 144)
    y, x = np.mgrid[0:height, 0:width]
    u = (x + 0.5) / width
    v = (y + 0.5) / height
    if flip:
        v = 1 - v
    clip = np.stack([u * 2 - 1, 1 - v * 2, depth, np.ones_like(depth)], axis=-1)
    h = clip @ rows.T
    p = h[..., :3] / h[..., 3:4]
    return p, np.linalg.norm(p - eye, axis=-1)


def compare_game(folder, info, world, sun, depth):
    mask_path, sun_path = os.path.join(folder, "mask.r8g8"), os.path.join(folder, "sun.bin")
    if not (os.path.exists(mask_path) and os.path.exists(sun_path)):
        report("INFO", "no game shadow mask or shadow-mask constants: no comparison with the game")
        return
    h, w = world.shape
    mask = np.fromfile(mask_path, np.uint8).reshape(h, w, 2)[..., 0] / 255.0
    constants = open(sun_path, "rb").read()
    shadow_distance = struct.unpack_from("<f", constants, 10 * 16)[0]
    b3 = 768 + 192 + 352
    spheres = [struct.unpack_from("<4f", constants, b3 + i * 16) for i in range(8)]
    reference = np.array([float(c) for c in info["reference"].split(",")])
    p, distance = scene_positions(folder, depth)
    world_points = p + reference
    inside = np.zeros_like(world)
    for cx, cy, cz, r2 in spheres:
        if np.isfinite([cx, cy, cz, r2]).all() and r2 > 0:
            inside |= ((world_points - np.array([cx, cy, cz])) ** 2).sum(axis=-1) <= r2
    pixels = world & ~np.isnan(sun) & (distance < shadow_distance) & inside
    n = pixels.sum()
    if n == 0:
        report("INFO", "no world pixel with a sun sample lies inside the game's cascades and shadow distance")
        return
    game_dark, traced_dark = mask < 0.5, sun < 0.5
    report("INFO", f"against the game's shadow mask, over {n} world pixels inside its cascades and {shadow_distance:.0f} m: game shadowed and "
                   f"traced lit {(pixels & game_dark & ~traced_dark).sum() / n:.4f} (casters the scene lacks; T1c past 0.02), game lit and traced "
                   f"shadowed {(pixels & ~game_dark & traced_dark).sum() / n:.4f}")


def compare_irradiance(folder, world, ao):
    path = os.path.join(folder, "irradiance.r11g11b10")
    if not os.path.exists(path):
        return
    h, w = world.shape
    rgb = r11g11b10(np.fromfile(path, np.uint32).reshape(h, w).astype(np.int64))
    luminance = rgb @ np.array([0.2126, 0.7152, 0.0722])
    valid = world & ~np.isnan(ao)
    if valid.sum() < 2 or np.std(ao[valid]) == 0 or np.std(luminance[valid]) == 0:
        return
    report("INFO", f"traced occlusion against the game's diffuse irradiance's luminance: correlation {np.corrcoef(ao[valid], luminance[valid])[0, 1]:.3f} "
                   f"over {valid.sum()} world pixels (information only)")


def main(folder):
    info = values(folder)
    w, h = int(info["width"]), int(info["height"])
    history = np.fromfile(os.path.join(folder, "history.f32"), np.float32).reshape(h, w, 4)
    reference_path = os.path.join(folder, "reference.f32")
    if not os.path.exists(reference_path):
        report("FAIL", f"{folder} has no reference.f32: run trace_reference on it first")
        return 1
    reference = np.fromfile(reference_path, np.float32).reshape(h, w, 4)
    stencil = np.fromfile(os.path.join(folder, "stencil.r8"), np.uint8).reshape(h, w)
    depth = np.fromfile(os.path.join(folder, "depth.r32"), np.float32).reshape(h, w)
    world = ((stencil & 0x84) == 0x80) & (depth > 0)
    report("INFO", f"{folder}: {w}x{h}, {world.sum()} world pixels, {info.get('passes', '?')} passes, scene {info.get('scene', '?')}")

    traced = world & ((history[..., 2] > 0) | (history[..., 3] > 0))
    same_counts = (history[..., 2:4] == reference[..., 2:4]).all(axis=-1)
    report("PASS" if same_counts[world].all() else "FAIL",
           f"1 the reference gave {same_counts[world].mean() if world.any() else 1:.6f} of the world pixels the Deck's sample counts ({traced.sum()} traced)")
    squares_path = os.path.join(folder, "reference-squares.f32")
    if not os.path.exists(squares_path):
        report("FAIL", f"{folder} has no reference-squares.f32: run this build's trace_reference on it")
        return 1
    squares = np.fromfile(squares_path, np.float32).reshape(h, w, 2).astype(np.float64)
    rows = []
    for which, name in ((0, "occlusion"), (1, "sun")):
        deck, mine = averages(history, which), averages(reference, which)
        valid = world & ~np.isnan(deck) & ~np.isnan(mine)
        if valid.sum() == 0:
            report("FAIL", f"2 {name}: no world pixel has samples")
            continue
        h64, r64 = history.astype(np.float64), reference.astype(np.float64)
        difference = sampling_checks(name, (h64[..., which], h64[..., 2 + which]), (r64[..., which], r64[..., 2 + which]), squares[..., which], valid)
        rows.append(np.concatenate([gray(np.where(world, deck, np.nan)), gray(np.where(world, mine, np.nan)), red(np.where(valid, difference, 0))], axis=1))
    if len(rows) == 2:
        write_png(os.path.join(folder, "check-trace.png"), np.concatenate(rows, axis=0))
    compare_game(folder, info, world, averages(history, 1), depth)
    compare_irradiance(folder, world, averages(history, 0))
    print(f"{folder}: {'all checks passed' if failures == 0 else 'some checks failed'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
