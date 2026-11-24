"""Exact thin-lens reference for CameraTools' depth of field on a synthetic scene of fronto-parallel layers.

    python3 lens_reference.py <out folder> [samples]

Writes <out>/frame (a frame folder render_dof reads: the pinhole picture, anti-aliased like SMAA, and its depth) and
<out>/ref.npy, the same scene through a thin lens: the average of pinhole renders from points spread over a round
aperture, each layer shifted by its signed CoC times the point, nearer layers covering farther ones. That is what a real
lens does with these layers, hidden surfaces included. <out>/layers.npy holds each pixel's sharp layer index and
<out>/depths.npy each layer's depth. 144 samples take about 7 minutes. MAPLE_Z moves the maple from 31 m.
"""
import os
import sys

import numpy as np

W, H, SS = 1280, 800, 2
P11 = 2.27267265
P10, P14 = 4.16636467e-05, 0.250010431
SENSOR, FNUM, FOCUS = 573.6, 1.2, 15.0
rng = np.random.default_rng(7)


def coc_px(z):
    """Signed full-size CoC radius in pixels, positive behind focus, as dof_coc reckons it."""
    f = SENSOR * 0.5 * P11
    scale_half = f * f / (FNUM * (FOCUS * 1000 - f)) / 2 * (H * 0.5 / SENSOR)
    return 2 * scale_half * (1 - FOCUS / z)


def quantize(rgb):
    """Through R11G11B10, as the game's HDR target stores it."""
    h = rgb.astype(np.float16).view(np.uint16).astype(np.uint32)
    r = ((h[..., 0] + 8) >> 4) & 0x7FF
    g = ((h[..., 1] + 8) >> 4) & 0x7FF
    b = ((h[..., 2] + 16) >> 5) & 0x3FF
    packed = r | g << 11 | b << 22
    back = np.stack([(r << 4), (g << 4), (b << 5)], -1).astype(np.uint16).view(np.float16).astype(np.float32)
    return packed.astype(np.uint32), back


class Layer:
    def __init__(self, z):
        self.z = z
        self.c = coc_px(min(z, P14 / P10))
        self.a = np.zeros((H * SS, W * SS), np.float32)
        self.rgb = np.zeros((H * SS, W * SS, 3), np.float32)

    def disk(self, cx, cy, r, color):
        x0, x1 = int(max((cx - r - 1) * SS, 0)), int(min((cx + r + 1) * SS, W * SS))
        y0, y1 = int(max((cy - r - 1) * SS, 0)), int(min((cy + r + 1) * SS, H * SS))
        if x0 >= x1 or y0 >= y1:
            return
        yy, xx = np.mgrid[y0:y1, x0:x1]
        m = ((xx + 0.5) / SS - cx) ** 2 + ((yy + 0.5) / SS - cy) ** 2 <= r * r
        self.a[y0:y1, x0:x1][m] = 1
        self.rgb[y0:y1, x0:x1][m] = color

    def hole(self, cx, cy, r):
        x0, x1 = int(max((cx - r - 1) * SS, 0)), int(min((cx + r + 1) * SS, W * SS))
        y0, y1 = int(max((cy - r - 1) * SS, 0)), int(min((cy + r + 1) * SS, H * SS))
        if x0 >= x1 or y0 >= y1:
            return
        yy, xx = np.mgrid[y0:y1, x0:x1]
        m = ((xx + 0.5) / SS - cx) ** 2 + ((yy + 0.5) / SS - cy) ** 2 <= r * r
        self.a[y0:y1, x0:x1][m] = 0

    def rect(self, x0, y0, x1, y1, color_fn):
        sx0, sx1, sy0, sy1 = int(x0 * SS), int(x1 * SS), int(y0 * SS), int(y1 * SS)
        yy, xx = np.mgrid[sy0:sy1, sx0:sx1]
        self.a[sy0:sy1, sx0:sx1] = 1
        self.rgb[sy0:sy1, sx0:sx1] = color_fn((xx + 0.5) / SS, (yy + 0.5) / SS)

    def blade(self, x0, y0, x1, y1, width, color):
        yy, xx = np.mgrid[0:H * SS, 0:W * SS]
        px, py = (xx + 0.5) / SS, (yy + 0.5) / SS
        dx, dy = x1 - x0, y1 - y0
        t = np.clip(((px - x0) * dx + (py - y0) * dy) / (dx * dx + dy * dy), 0, 1)
        m = np.hypot(px - x0 - t * dx, py - y0 - t * dy) <= width / 2
        self.a[m] = 1
        self.rgb[m] = color


def build():
    yy, xx = np.mgrid[0:H * SS, 0:W * SS]
    px, py = (xx + 0.5) / SS, (yy + 0.5) / SS
    sky = Layer(1e9)
    k = py / H
    clouds = 0.25 * (np.sin(px / 47 + np.sin(py / 31)) * np.sin(py / 23 + px / 90) + 1)
    sky.rgb[:] = np.stack([0.55 + 0.6 * k + clouds, 0.75 + 0.5 * k + clouds, 1.1 + 0.3 * k + clouds], -1) * 1.1
    sky.a[:] = 1
    for _ in range(10):
        sky.disk(rng.uniform(100, 1200), rng.uniform(30, 300), 1.2, (6.0, 5.0, 3.5))
    hills = Layer(300.0)
    horizon = 470 + 30 * np.sin(px / 140) + 12 * np.sin(px / 37)
    m = py > horizon
    tex = 0.5 + 0.5 * np.sign(np.sin(px / 3.1) * np.sin(py / 2.7))
    hills.a[m] = 1
    hills.rgb[m] = (np.stack([0.18 + 0.25 * tex, 0.3 + 0.3 * tex, 0.15 + 0.1 * tex], -1))[m]
    maple = Layer(float(os.environ.get("MAPLE_Z", "31")))
    for _ in range(700):
        cx, cy = rng.uniform(760, 1290), rng.uniform(-10, 330)
        v = rng.uniform(0.6, 1.4)
        maple.disk(cx, cy, rng.uniform(3, 12), (0.35 * v, 0.06 * v, 0.07 * v))
    for _ in range(500):
        maple.hole(rng.uniform(760, 1290), rng.uniform(-10, 330), rng.uniform(0.8, 2.5))
    maple.blade(1000, 330, 1030, 120, 9, (0.12, 0.08, 0.06))
    banner = Layer(45.0)
    banner.rect(1150, 40, 1192, 260, lambda x, y: np.stack([0.12 + 0 * x, 0.12 + 0 * x, 0.45 + 0.1 * (np.floor(y / 9) % 2)], -1))
    leaves = Layer(18.0)
    for _ in range(260):
        v = rng.uniform(0.6, 1.4)
        leaves.disk(rng.uniform(800, 1120), rng.uniform(40, 280), rng.uniform(2, 6), (0.4 * v, 0.1 * v, 0.06 * v))
    fence = Layer(17.0)
    fence.rect(120, 300, 420, 470, lambda x, y: np.stack([0.5 + 0.4 * (np.floor(x / 6) % 2)] * 3, -1) * np.array([0.9, 0.8, 0.6]))
    for i in range(8):
        fence.hole(140 + i * 38, 330, 9)
    subject = Layer(15.0)
    subject.rect(480, 360, 720, 560, lambda x, y: np.stack([0.3 + 0.6 * ((np.floor(x / 2) + np.floor(y / 2)) % 2), 0.25 + 0.3 * (np.floor(y / 5) % 2), 0.2 + 0.0 * x], -1))
    subject.rect(898, 80, 902, 600, lambda x, y: np.stack([0.9 + 0 * x, 0.85 + 0 * x, 0.2 + 0 * x], -1))
    subject.disk(800, 420, 26, (0.9, 0.7, 0.6))
    front = Layer(13.5)
    front.rect(300, 520, 470, 700, lambda x, y: np.stack([0.6 + 0.3 * (np.floor(y / 4) % 2), 0.55 + 0 * x, 0.5 + 0 * x], -1))
    near = Layer(8.0)
    for i in range(7):
        near.blade(420 + i * 45, 800, 520 + i * 30, 430 + 25 * i, 4, (0.2, 0.55 - 0.03 * i, 0.15))
    for _ in range(60):
        near.disk(rng.uniform(-20, 220), rng.uniform(-20, 200), rng.uniform(5, 14), (0.15, 0.3, 0.1))
    layers = [sky, hills, banner, maple, leaves, fence, subject, front, near]
    for layer in layers:
        layer.rgb = quantize(layer.rgb)[1] * layer.a[..., None]
    return layers


def sample(img, x, y):
    """Bilinear at full-size pixel coordinates (centers at .5) from a 2x raster, zero outside."""
    sx, sy = x * SS - 0.5, y * SS - 0.5
    x0, y0 = np.floor(sx).astype(np.int32), np.floor(sy).astype(np.int32)
    fx, fy = (sx - x0).astype(np.float32), (sy - y0).astype(np.float32)
    h, w = img.shape[:2]
    out = 0
    for dy, wy in ((0, 1 - fy), (1, fy)):
        for dx, wx in ((0, 1 - fx), (1, fx)):
            xi, yi = x0 + dx, y0 + dy
            ok = (xi >= 0) & (xi < w) & (yi >= 0) & (yi < h)
            v = img[np.clip(yi, 0, h - 1), np.clip(xi, 0, w - 1)]
            wgt = (wx * wy * ok)
            out = out + (v * (wgt[..., None] if v.ndim == 3 else wgt))
    return out


def render(layers, offsets, jitters):
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    acc = np.zeros((H, W, 3), np.float32)
    for (ux, uy), (jx, jy) in zip(offsets, jitters):
        col = np.zeros((H, W, 3), np.float32)
        for layer in layers:
            x, y = xx + 0.5 + jx - layer.c * ux, yy + 0.5 + jy - layer.c * uy
            a = sample(layer.a, x, y)
            col = col * (1 - a[..., None]) + sample(layer.rgb, x, y)
        acc += col
    return acc / len(offsets)


def aperture(n):
    side = int(np.sqrt(n))
    pts = []
    for i in range(side):
        for j in range(side):
            a, b = 2 * (i + rng.uniform()) / side - 1, 2 * (j + rng.uniform()) / side - 1
            if a == 0 and b == 0:
                pts.append((0.0, 0.0))
            elif abs(a) > abs(b):
                r, t = a, np.pi / 4 * b / a
                pts.append((r * np.cos(t), r * np.sin(t)))
            else:
                r, t = b, np.pi / 2 - np.pi / 4 * a / b
                pts.append((r * np.cos(t), r * np.sin(t)))
    return pts


def main():
    out = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 256
    os.makedirs(os.path.join(out, "frame"), exist_ok=True)
    layers = build()
    grid = [((i + 0.5) / 4 - 0.5, (j + 0.5) / 4 - 0.5) for i in range(4) for j in range(4)]
    sharp = render(layers, [(0.0, 0.0)] * 16, grid)
    packed, _ = quantize(sharp)
    index = np.zeros((H, W), np.int32)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    for i, layer in enumerate(layers):
        index[sample(layer.a, xx + 0.5, yy + 0.5) >= 0.5] = i
    z = np.array([l.z for l in layers])[index]
    depth = np.where(z > 1e5, 0.0, P14 / z - P10).astype(np.float32)
    frame = os.path.join(out, "frame")
    packed[::-1].astype("<u4").tofile(os.path.join(frame, "hdr.r11g11b10"))
    depth[::-1].astype("<f4").tofile(os.path.join(frame, "depth.r32"))
    with open(os.path.join(frame, "frame.tsv"), "w") as f:
        f.write(f"width\t{W}\nheight\t{H}\nflip\t1\nframe\t1\n")
        f.write("world_to_view\t1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1\n")
        f.write(f"view_to_clip\t{P11 / 1.6},0,0,0,0,{P11},0,0,0,0,{P10},-1,0,0,{P14},0\nnear\t0.25\nfar\t6000\n")
    jit = [tuple(rng.uniform(-0.5, 0.5, 2)) for _ in range(n)]
    ref = render(layers, aperture(n), jit)
    np.save(os.path.join(out, "ref.npy"), ref)
    np.save(os.path.join(out, "layers.npy"), index)
    np.save(os.path.join(out, "depths.npy"), np.array([l.z for l in layers]))
    print("CoC px by layer:", ", ".join(f"{l.z:g} m {l.c:+.1f}" for l in layers))


main()
