"""How many lens samples a lens-sampled screenshot needs, and how much each sample's own blur fills the gaps between them
(docs/genshin-lens-sampled-dof-plan.md in the melonloader workspace).

    python3 lens_hybrid.py <out folder>

A small scene of fronto-parallel layers: bright lamps on a background blurred by 40 pixels, leaves with gaps blurred by
14, and a pole in focus. It renders the scene through a round thin lens from aperture points on CameraTools' rings
(CameraTools/LensSampler.cs) or on a Vogel spiral, each sample's layers optionally blurred first by a disc of a share k of
their own CoC, as each sample's depth of field would, and compares each with 4096 points as the exact picture: the
error in 8-bit levels through the game's tone curve, and how much the lamps' discs ripple with the copies the points
leave. Writes the pictures as PNGs.
"""
import os
import sys

import numpy as np

W, H = 384, 256
rng = np.random.default_rng(3)
GOLDEN = np.pi * (3 - np.sqrt(5))


def tone(x, exposure=0.985):
    x = np.maximum(x * exposure, 0)
    c = np.clip(x * (1.36 * x + 0.047) / (x * (0.93 * x + 0.56) + 0.14), 0, 1)
    return np.clip(np.maximum(np.power(c, 1 / 2.4) * 1.055 - 0.055, 0), 0, 1) * 255


def rings(n):
    """LensSampler.Points for a round aperture with no edge softness."""
    pts = [(0.0, 0.0)]
    count_rings = max(1, int(round(np.sqrt(n / np.pi))))
    left, total, at = n - 1, count_rings * (count_rings + 1) // 2, 1
    for ring in range(1, count_rings + 1):
        k = left if ring == count_rings else min(left, max(1, int(round((n - 1) * ring / total))))
        left -= k
        q = np.sqrt((at + 0.5 * k) / n)
        turn = ring * GOLDEN / (2 * np.pi)
        for i in range(k):
            a = 2 * np.pi * ((i + 0.5) / k + turn)
            pts.append((q * np.cos(a), q * np.sin(a)))
        at += k
    return np.array(pts[:n])


def vogel(n):
    i = np.arange(n)
    r = np.sqrt((i + 0.5) / n)
    return np.stack([r * np.cos(i * GOLDEN), r * np.sin(i * GOLDEN)], -1)


def disc(radius):
    r = int(np.ceil(radius)) + 1
    y, x = np.mgrid[-r:r + 1, -r:r + 1]
    k = np.clip(radius + 0.5 - np.hypot(x, y), 0, 1)
    return k / k.sum()


def blur(img, radius):
    """img convolved with a disc of radius pixels, zero outside."""
    if radius < 0.25:
        return img
    k = disc(radius)
    ph, pw = img.shape[0] + k.shape[0], img.shape[1] + k.shape[1]
    kf = np.fft.rfft2(k, (ph, pw))
    out = np.fft.irfft2(np.fft.rfft2(img, (ph, pw), axes=(0, 1)) * (kf[..., None] if img.ndim == 3 else kf), (ph, pw), axes=(0, 1))
    oy, ox = k.shape[0] // 2, k.shape[1] // 2
    return out[oy:oy + img.shape[0], ox:ox + img.shape[1]]


def scene():
    """Layers far to near: (CoC in pixels, premultiplied rgb, alpha)."""
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    sky = np.stack([0.35 + 0.3 * yy / H, 0.45 + 0.25 * yy / H, 0.7 + 0.1 * yy / H], -1) + 0.08 * np.sin(xx / 9)[..., None]
    for _ in range(14):
        cx, cy = rng.uniform(30, W - 30), rng.uniform(30, H - 30)
        sky += (300.0 * np.exp(-((xx - cx) ** 2 + (yy - cy) ** 2) / (2 * 1.2 ** 2)))[..., None] * np.array([1.0, 0.85, 0.6])
    leaves_a = np.zeros((H, W), np.float32)
    for _ in range(140):
        cx, cy, r = rng.uniform(0, W), rng.uniform(0, H * 0.6), rng.uniform(3, 9)
        leaves_a[(xx - cx) ** 2 + (yy - cy) ** 2 <= r * r] = 1
    for _ in range(90):
        cx, cy, r = rng.uniform(0, W), rng.uniform(0, H * 0.6), rng.uniform(1, 3)
        leaves_a[(xx - cx) ** 2 + (yy - cy) ** 2 <= r * r] = 0
    leaves = leaves_a[..., None] * np.array([0.3, 0.05, 0.04])
    pole_a = ((np.abs(xx - 250) <= 1.5) & (yy > 40)).astype(np.float32)
    pole = pole_a[..., None] * np.array([0.9, 0.8, 0.3])
    return [(40.0, sky, np.ones((H, W), np.float32)), (14.0, leaves, leaves_a), (0.0, pole, pole_a)]


def shift(img, dx, dy):
    """img moved by (dx, dy) pixels, bilinear, zero outside."""
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    sx, sy = xx - dx, yy - dy
    x0, y0 = np.floor(sx).astype(int), np.floor(sy).astype(int)
    fx, fy = sx - x0, sy - y0
    out = 0
    for oy, wy in ((0, 1 - fy), (1, fy)):
        for ox, wx in ((0, 1 - fx), (1, fx)):
            xi, yi = x0 + ox, y0 + oy
            ok = (xi >= 0) & (xi < W) & (yi >= 0) & (yi < H)
            v = img[np.clip(yi, 0, H - 1), np.clip(xi, 0, W - 1)]
            w = wx * wy * ok
            out = out + v * (w[..., None] if v.ndim == 3 else w)
    return out


def render(layers, points, k):
    pre = [(c, blur(rgb, k * c), blur(a, k * c)) for c, rgb, a in layers]
    acc = np.zeros((H, W, 3))
    for px, py in points:
        jx, jy = rng.uniform(-0.5, 0.5, 2)
        col = np.zeros((H, W, 3))
        for c, rgb, a in pre:
            dx, dy = c * px + jx, c * py + jy
            sa = shift(a, dx, dy)
            col = col * (1 - sa[..., None]) + shift(rgb, dx, dy)
        acc += col
    return acc / len(points)


def render_sky_mean(layers):
    """The sky without its lamps, which the lamps' discs stand out of."""
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    return np.stack([0.35 + 0.3 * yy / H, 0.45 + 0.25 * yy / H, 0.7 + 0.1 * yy / H], -1)


def ripple(img, mask):
    """The copies' ripple: the mean of the picture less its 5x5 mean, inside mask."""
    pad = np.pad(img, ((2, 2), (2, 2), (0, 0)), mode="edge")
    mean = sum(pad[y:y + H, x:x + W] for y in range(5) for x in range(5)) / 25
    return np.abs(img - mean).mean(-1)[mask].mean()


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    layers = scene()
    fill = lambda n: 0.5 * np.sqrt(np.pi / n)
    exact = tone(render(layers, rings(4096), fill(4096)))
    # The lamps' discs: where the exact picture is brighter than the sky by a quarter of the range.
    lamps = exact.mean(-1) - tone(render_sky_mean(layers)).mean(-1) > 40
    from PIL import Image
    Image.fromarray(exact.astype(np.uint8)).save(os.path.join(out, "exact.png"))
    runs = [("rings 256", rings(256), 0.0), ("spiral 256", vogel(256), 0.0), ("rings 256, fill", rings(256), fill(256)),
            ("rings 256, fill x2", rings(256), 2 * fill(256)), ("rings 128, fill", rings(128), fill(128)), ("rings 512, fill", rings(512), fill(512))]
    print(f"{'':24s}{'mean':>8s}{'p99':>8s}{'lamps':>8s}{'ripple':>8s}   exact ripple {ripple(exact, lamps):.2f}")
    for name, pts, k in runs:
        img = tone(render(layers, pts, k))
        d = np.abs(img - exact).mean(-1)
        print(f"{name:24s}{d.mean():8.2f}{np.percentile(d, 99):8.1f}{d[lamps].mean():8.2f}{ripple(img, lamps):8.2f}")
        Image.fromarray(img.astype(np.uint8)).save(os.path.join(out, name.replace(" ", "-").replace(",", "") + ".png"))


main()
