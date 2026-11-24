"""Errors of render_dof PNGs against lens_reference.py's thin-lens reference, in 8-bit levels of the game's tone curve,
by region, as mean and 99th percentile. Each most:<region>=<levels> fails the run when a PNG's mean there exceeds it.

    python3 lens_compare.py <reference folder> <label>=<png> ... [most:<region>=<levels> ...]
"""
import sys

import numpy as np
from PIL import Image


def tone(x, exposure=0.985):
    x = np.maximum(x * exposure, 0)
    c = np.clip(x * (1.36 * x + 0.047) / (x * (0.93 * x + 0.56) + 0.14), 0, 1)
    return np.clip(np.maximum(np.power(c, 1 / 2.4) * 1.055 - 0.055, 0), 0, 1) * 255


def dilate(m, r):
    for _ in range(r):
        p = np.pad(m, 1)
        m = p[1:-1, 1:-1] | p[:-2, 1:-1] | p[2:, 1:-1] | p[1:-1, :-2] | p[1:-1, 2:]
    return m


out = sys.argv[1]
ref = tone(np.load(f"{out}/ref.npy"))
index = np.load(f"{out}/layers.npy")
z = np.load(f"{out}/depths.npy")
n = len(z)
SKY, MAPLE, FOCUS = 0, int(np.argmin(np.abs(z - 31))), int(np.argmin(np.abs(z - 15)))
FG = int(np.argmin(z))
near = {i: dilate(index == i, 18) for i in range(n)}
count = sum(near[i].astype(int) for i in range(n))
fg = dilate(index == FG, 17)
far = z[index] > 16
border = np.zeros_like(fg)
border[20:-20, 20:-20] = True
fg &= border
far &= border
regions = {
    "in focus, inside": (index == FOCUS) & ~dilate(index != FOCUS, 3),
    "in focus, edges": (np.abs(z[index] - 15) < 2.5) & ~fg & (count > 1) & border,
    "far, inside": far & ~fg & (count == 1),
    "far, layer edges": far & ~fg & (count > 1),
    "sky in leaf gaps": (index == SKY) & dilate(index == MAPLE, 3) & ~fg,
    "foreground blur": fg,
    "whole frame": border,
}
if (np.abs(z - 18) < 0.5).any():
    LEAVES, BANNER = int(np.argmin(np.abs(z - 18))), int(np.argmin(np.abs(z - 45)))
    regions["near leaves on leaves"] = dilate(index == LEAVES, 4) & border
    regions["banner and rim"] = dilate(index == BANNER, 6) & border
print(f"{'':26s}" + "".join(f"{name[:17]:>19s}" for name in regions))
most = {}
for arg in sys.argv[2:]:
    if arg.startswith("most:"):
        name, value = arg[5:].rsplit("=", 1)
        most[name] = float(value)
failed = []
for arg in sys.argv[2:]:
    if arg.startswith("most:"):
        continue
    label, path = arg.split("=", 1)
    img = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    d = np.abs(img - ref).mean(-1)
    cells = []
    for name, m in regions.items():
        cells.append(f"{d[m].mean():6.2f} p99 {np.percentile(d[m], 99):5.1f}")
        if name in most and d[m].mean() > most[name]:
            failed.append(f"{label}: {name} {d[m].mean():.2f} levels, at most {most[name]}")
    print(f"{label:26s}" + "".join(f"{c:>19s}" for c in cells))
for line in failed:
    print("FAIL", line)
sys.exit(1 if failed else 0)
