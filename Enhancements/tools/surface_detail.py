#!/usr/bin/env python3
"""Pack an ambientCG material into the Enhancements add-on's surface detail map.

Usage:
  surface_detail.py <folder> <out.dds>

<folder> holds an ambientCG download (CC0), such as Rock030_1K-JPG: its *_NormalGL.jpg, *_Color.jpg and *_Roughness.jpg.
The DDS is RGBA8 with every mip: r and g the OpenGL tangent-space normal's x and y, b the color's luminance scaled so its
mean is 0.5, a the roughness. The add-on reads it from ReShade's Enhancements\\surface-detail\\rock.dds. Keep the maps
out of the repository, as the custom grass's are.
"""

import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def find(folder, suffix):
    found = sorted(Path(folder).glob(f"*_{suffix}.jpg")) + sorted(Path(folder).glob(f"*_{suffix}.png"))
    if not found:
        sys.exit(f"no *_{suffix} map in {folder}")
    return Image.open(found[0])


def levels(image):
    """The image's mips from the full size down to 1 x 1, by 2 x 2 averages. Averaged normals shorten, which flattens the
    detail with distance as it should."""
    out = [image]
    while image.shape[0] > 1 or image.shape[1] > 1:
        h, w = max(1, image.shape[0] // 2), max(1, image.shape[1] // 2)
        image = image.reshape(h, image.shape[0] // h, w, image.shape[1] // w, 4).mean(axis=(1, 3))
        out.append(image)
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    folder, out = sys.argv[1], sys.argv[2]
    normal = np.asarray(find(folder, "NormalGL").convert("RGB"), dtype=np.float32) / 255
    color = np.asarray(find(folder, "Color").convert("RGB"), dtype=np.float32) / 255
    rough = np.asarray(find(folder, "Roughness").convert("L"), dtype=np.float32) / 255
    luminance = color @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    shade = np.clip(luminance / max(float(luminance.mean()), 1e-4) * 0.5, 0, 1)
    image = np.dstack([normal[..., 0], normal[..., 1], shade, rough])
    mips = levels(image)
    height, width = image.shape[:2]
    header = bytearray(148)
    struct.pack_into('<4sIIIIIII', header, 0, b'DDS ', 124, 0x21007, height, width, width * 4, 0, len(mips))
    struct.pack_into('<II4s', header, 76, 32, 4, b'DX10')
    struct.pack_into('<IIIII', header, 128, 28, 3, 0, 1, 0)
    with open(out, 'wb') as o:
        o.write(header)
        for mip in mips:
            o.write((np.clip(mip, 0, 1) * 255 + 0.5).astype(np.uint8).tobytes())
    print(f"{out}: {width} x {height}, {len(mips)} mips, shade mean {float(shade.mean()):.3f}")


if __name__ == "__main__":
    main()
