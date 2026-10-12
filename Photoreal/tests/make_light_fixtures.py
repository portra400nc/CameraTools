#!/usr/bin/env python3
"""Write light_test.cpp's fixtures, and print the values it expects.

python3 make_light_fixtures.py <FrameCensus logs folder> <Photoreal logs folder> <fixtures folder>

From census captures 111824 and 214859: the pixel shader constant buffers b0 and b3 of the combine step's first draw,
the first draw after the clear of the HDR scene, as <capture>-combine-b0.bin and -b3.bin, and their row 0.

From build 208's meadow trace trace-20261012-180456: every 37th world pixel's G-buffer normal and diffuse irradiance,
as they were stored, in trace-180456-ambient.bin: b"PRAF", a uint32 count, then per pixel the R10G10B10A2_UNORM normal
and the R11G11B10_FLOAT irradiance, 4 bytes each. Then the first-order least-squares fit of the irradiance to the
normalized normal over those pixels, which fit_ambient must give.
"""
import os
import struct
import sys

import numpy as np

CAPTURES = ("capture-20261010-111824", "capture-20261009-214859")
TRACE = "trace-20261012-180456"
STRIDE = 37


def combine_constants(capture):
    rows = [line.rstrip("\n").split("\t") for line in open(os.path.join(capture, "events.tsv"))]
    head = rows[0]
    column = {name: i for i, name in enumerate(head)}
    for i, row in enumerate(rows[1:], 1):
        if row[column["event"]] == "clear_rtv" and "R11G11B10_FLOAT" in row[column["rtvs"]] and row[column["detail"]].startswith("color=0.03"):
            break
    else:
        sys.exit(f"{capture}: no combine clear")
    for row in rows[i + 1:]:
        if row[column["event"]].startswith("draw"):
            buffers = {c.split(":")[0]: c.split(":")[1] for c in row[column["cbuffers"]].split(",") if c.startswith("psb")}
            return [open(os.path.join(capture, "cbuffers", buffers[f"psb{b}"] + ".bin"), "rb").read() for b in (0, 3)]
    sys.exit(f"{capture}: no draw after the combine clear")


def small_float(bits, mantissa_bits):
    e = (bits >> mantissa_bits) & 31
    m = (bits & ((1 << mantissa_bits) - 1)).astype(np.float64) / (1 << mantissa_bits)
    return np.where(e == 0, np.ldexp(m, -14), np.ldexp(1 + m, e.astype(np.int64) - 15))


def main(census, photoreal, fixtures):
    for name in CAPTURES:
        b0, b3 = combine_constants(os.path.join(census, name))
        open(os.path.join(fixtures, f"{name}-combine-b0.bin"), "wb").write(b0)
        open(os.path.join(fixtures, f"{name}-combine-b3.bin"), "wb").write(b3)
        print(f"{name}: b0 {len(b0)} bytes, row 0 {struct.unpack_from('<4f', b0)}; b3 {len(b3)} bytes, row 0 {struct.unpack_from('<4f', b3)}")

    folder = os.path.join(photoreal, TRACE)
    info = dict(line.rstrip("\n").split("\t", 1) for line in open(os.path.join(folder, "trace.tsv")))
    w, h = int(info["width"]), int(info["height"])
    stencil = np.fromfile(os.path.join(folder, "stencil.r8"), np.uint8)
    depth = np.fromfile(os.path.join(folder, "depth.r32"), np.float32)
    normals = np.fromfile(os.path.join(folder, "normals.r10g10b10a2"), np.uint32)
    irradiance = np.fromfile(os.path.join(folder, "irradiance.r11g11b10"), np.uint32)
    world = np.flatnonzero(((stencil & 0x84) == 0x80) & (depth > 0))[::STRIDE]
    with open(os.path.join(fixtures, "trace-180456-ambient.bin"), "wb") as out:
        out.write(b"PRAF" + struct.pack("<I", len(world)))
        out.write(np.stack([normals[world], irradiance[world]], axis=1).astype("<u4").tobytes())
    n = np.stack([(normals[world] >> (10 * k)) & 1023 for k in range(3)], axis=1) / 1023.0 * 2 - 1
    n /= np.linalg.norm(n, axis=1, keepdims=True)
    words = irradiance[world].astype(np.int64)
    rgb = np.stack([small_float(words & 0x7FF, 6), small_float((words >> 11) & 0x7FF, 6), small_float(words >> 22, 5)], axis=1)
    basis = np.c_[np.ones(len(n)), n]
    coefficients, *_ = np.linalg.lstsq(basis, rgb, rcond=None)
    residual = rgb - basis @ coefficients
    r2 = 1 - (residual ** 2).sum(0) / ((rgb - rgb.mean(0)) ** 2).sum(0)
    print(f"{TRACE}: {len(world)} of {w}x{h}'s world pixels; fit rows (1, x, y, z) by rgb:")
    for row in coefficients:
        print("  {%.6ff, %.6ff, %.6ff}" % tuple(row))
    print(f"  r2 {np.round(r2, 5)}; the mean of the first 100: {np.round(rgb[:100].mean(0), 6)}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
