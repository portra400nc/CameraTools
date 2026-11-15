"""Builds the night sky's tables from their public sources, which SOURCES.txt names: the stars from the Yale Bright Star
Catalog and the moon's face from the LRO color map. Run with the downloaded files:

    python3 -I make_sky_data.py <bsc5.dat.gz> <lroc_color_poles_2k.tif> <out folder>

stars.bin: per star six little-endian float32s, the J2000 equatorial unit vector (x toward the March equinox, z north) and
the star's light in linear sRGB, in units of the sun's light above the atmosphere, colored relative to the sun's, which
the sky model takes as white.
moon.bin: the near side, longitude -90 to 90 left to right and latitude 90 to -90 top to bottom, 512 x 512 bytes, the
albedo over its mean on the disc as seen from the earth, over 3 and gamma 2.2 encoded; then 64 x 64 x 3 bytes of its
color over its luminance, halved, linear.
"""
import gzip
import struct
import sys

import numpy as np
from PIL import Image

SUN_V = -26.74
SUN_B_V = 0.65


def temperature(b_v):
    # Ballesteros (2012), a black body that matches a star's B-V.
    return 4600 * (1 / (0.92 * b_v + 1.7) + 1 / (0.92 * b_v + 0.62))


def cie_xyz(nm):
    # Wyman, Sloan and Shirley (2013), the CIE 1931 observer as a sum of Gaussians.
    def g(x, mu, s1, s2):
        s = np.where(x < mu, s1, s2)
        return np.exp(-0.5 * ((x - mu) / s) ** 2)
    x = 1.056 * g(nm, 599.8, 37.9, 31.0) + 0.362 * g(nm, 442.0, 16.0, 26.7) - 0.065 * g(nm, 501.1, 20.4, 26.2)
    y = 0.821 * g(nm, 568.8, 46.9, 40.5) + 0.286 * g(nm, 530.9, 16.3, 31.1)
    z = 1.217 * g(nm, 437.0, 11.8, 36.0) + 0.681 * g(nm, 459.0, 26.0, 13.8)
    return x, y, z


NM = np.arange(380.0, 781.0, 5.0)
XYZ_TO_SRGB = np.array([[3.2406, -1.5372, -0.4986], [-0.9689, 1.8758, 0.0415], [0.0557, -0.2040, 1.0570]])


def black_body_rgb(t):
    m = NM * 1e-9
    planck = 1 / (m ** 5 * (np.exp(1.4388e-2 / (m * t)) - 1))
    x, y, z = cie_xyz(NM)
    rgb = XYZ_TO_SRGB @ np.array([(planck * x).sum(), (planck * y).sum(), (planck * z).sum()])
    return np.maximum(rgb, 0)


def luminance(rgb):
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]


def stars(path):
    sun = black_body_rgb(temperature(SUN_B_V))
    out = []
    with gzip.open(path, "rt", encoding="ascii") as f:
        for line in f:
            line = line.rstrip("\n").ljust(197)
            if not line[75:77].strip() or not line[102:107].strip():
                continue
            ra = (int(line[75:77]) + int(line[77:79]) / 60 + float(line[79:83]) / 3600) * np.pi / 12
            dec = (int(line[84:86]) + int(line[86:88]) / 60 + int(line[88:90]) / 3600) * np.pi / 180
            if line[83] == "-":
                dec = -dec
            v = float(line[102:107])
            b_v = float(line[109:114]) if line[109:114].strip() else SUN_B_V
            color = black_body_rgb(temperature(min(max(b_v, -0.4), 2.0))) / sun
            color /= luminance(color)
            flux = color * 10 ** (-0.4 * (v - SUN_V))
            out.append((np.cos(dec) * np.cos(ra), np.cos(dec) * np.sin(ra), np.sin(dec), *flux))
    return out


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def moon(path):
    image = srgb_to_linear(np.asarray(Image.open(path).convert("RGB"), dtype=np.float64) / 255)
    h, w, _ = image.shape
    near = image[:, w // 4: 3 * w // 4]
    lum = luminance(near.transpose(2, 0, 1))
    rows, cols = lum.shape
    lat = (0.5 - (np.arange(rows) + 0.5) / rows) * np.pi
    lon = ((np.arange(cols) + 0.5) / cols - 0.5) * np.pi
    # Seen from the earth a texel covers its area times the cosine of its angle from the disc's center.
    weight = np.cos(lat)[:, None] ** 2 * np.cos(lon)[None, :]
    mean = (lum * weight).sum() / weight.sum()

    def shrink(a, size):
        f = a.shape[0] // size
        return a.reshape(size, f, size, f, *a.shape[2:]).mean(axis=(1, 3))

    gray = shrink(lum / mean, 512)
    encoded = np.round(255 * np.clip(gray / 3, 0, 1) ** (1 / 2.2)).astype(np.uint8)
    small = shrink(near, 64)
    chroma = small / np.maximum(luminance(small.transpose(2, 0, 1)), 1e-6)[..., None]
    chroma = np.round(255 * np.clip(chroma / 2, 0, 1)).astype(np.uint8)
    return encoded.tobytes() + chroma.tobytes(), mean


def main():
    catalog, moon_map, out = sys.argv[1:4]
    table = stars(catalog)
    with open(f"{out}/stars.bin", "wb") as f:
        for row in table:
            f.write(struct.pack("<6f", *row))
    data, mean = moon(moon_map)
    with open(f"{out}/moon.bin", "wb") as f:
        f.write(data)
    print(f"{len(table)} stars; the moon map's mean linear value on the disc {mean:.4f}")


main()
