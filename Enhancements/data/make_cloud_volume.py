"""Converts the Walt Disney Animation Studios Cloud Data Set's eighth-resolution cloud (wdas_cloud_eighth.vdb, from
https://disneyanimation.com/resources/clouds/) to wdas_cloud/wdas_cloud_eighth.r8: its density over the file's bounding
box, 250 x 170 x 307 voxels, one byte each, x fastest, then y (up), then z. The volume clouds stand towers of it.

python3 -I make_cloud_volume.py path/to/wdas_cloud_eighth.vdb

The data is Copyright 2017 Disney Enterprises, Inc., CC BY-SA 3.0 (wdas_cloud/LICENSE.txt). This reader handles only
what the cloud's files use: one float grid, file version 222 and up, active-mask compression, full floats.
"""
import struct
import sys

import numpy as np

COMPRESS_ZIP, COMPRESS_ACTIVE_MASK, COMPRESS_BLOSC = 1, 2, 4


class Reader:
    def __init__(self, data):
        self.d, self.p = data, 0

    def take(self, n):
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def u32(self): return struct.unpack('<I', self.take(4))[0]
    def i32(self): return struct.unpack('<i', self.take(4))[0]
    def i64(self): return struct.unpack('<q', self.take(8))[0]
    def f32(self): return struct.unpack('<f', self.take(4))[0]
    def u8(self): return self.take(1)[0]
    def s8(self): return struct.unpack('<b', self.take(1))[0]
    def string(self): return self.take(self.u32()).decode('latin-1')

    def meta(self):
        out = {}
        for _ in range(self.u32()):
            name, kind = self.string(), self.string()
            raw = self.take(self.u32())
            out[name] = (kind, raw)
        return out


def mask(r, bits):
    raw = np.frombuffer(r.take(bits // 8), dtype=np.uint8)
    return np.unpackbits(raw, bitorder='little').astype(bool)


def values(r, count, value_mask, compression, background):
    meta = r.s8()
    inactive0 = background if meta == 0 else -background
    inactive1 = background
    if meta in (2, 4, 5):
        inactive0 = r.f32()
        if meta == 5:
            inactive1 = r.f32()
    selection = mask(r, count) if meta in (3, 4, 5) else None
    n = count
    if compression & COMPRESS_ACTIVE_MASK and meta != 6:
        n = int(value_mask.sum())
    if compression & (COMPRESS_ZIP | COMPRESS_BLOSC):
        raise SystemExit('zip/blosc compression is not supported')
    vals = np.frombuffer(r.take(n * 4), dtype='<f4')
    if n == count:
        return vals.copy()
    out = np.where(selection, inactive1, inactive0).astype(np.float32) if selection is not None else np.full(count, inactive0, np.float32)
    out[value_mask] = vals
    return out



def read_volume(path):
    r = Reader(open(path, 'rb').read())
    assert r.i64() == 0x56444220, 'not a VDB file'
    version = r.u32()
    r.u32(); r.u32()
    has_offsets = r.u8()
    r.take(36)
    r.meta()
    assert version >= 222 and has_offsets and r.u32() >= 1
    r.string(); r.string(); r.string()
    grid_pos, block_pos, end_pos = r.i64(), r.i64(), r.i64()
    r.p = grid_pos
    compression = r.u32()
    grid_meta = r.meta()
    sizes = {'UniformScaleMap': 5 * 24, 'ScaleMap': 5 * 24, 'UniformScaleTranslateMap': 6 * 24, 'ScaleTranslateMap': 6 * 24}
    r.take(sizes[r.string()])
    assert r.i32() == 1
    background = r.f32()
    tiles, children = r.u32(), r.u32()
    leaves, const = [], []
    for _ in range(tiles):
        o = struct.unpack('<3i', r.take(12)); v = r.f32(); a = r.u8()
        if a and v != 0:
            const.append((o, 4096, v))
    for _ in range(children):
        o2 = np.array(struct.unpack('<3i', r.take(12)))
        cm2, vm2 = mask(r, 32768), mask(r, 32768)
        v2 = values(r, 32768, vm2, compression, background)
        for n in np.nonzero(vm2 & ~cm2)[0]:
            if v2[n] != 0:
                const.append((o2 + np.array([n >> 10, (n >> 5) & 31, n & 31]) * 128, 128, v2[n]))
        for n in np.nonzero(cm2)[0]:
            o1 = o2 + np.array([n >> 10, (n >> 5) & 31, n & 31]) * 128
            cm1, vm1 = mask(r, 4096), mask(r, 4096)
            v1 = values(r, 4096, vm1, compression, background)
            for m in np.nonzero(vm1 & ~cm1)[0]:
                if v1[m] != 0:
                    const.append((o1 + np.array([m >> 8, (m >> 4) & 15, m & 15]) * 8, 8, v1[m]))
            for m in np.nonzero(cm1)[0]:
                r.take(64)
                leaves.append(o1 + np.array([m >> 8, (m >> 4) & 15, m & 15]) * 8)
    r.p = block_pos
    lo = np.array(struct.unpack('<3i', grid_meta['file_bbox_min'][1]))
    hi = np.array(struct.unpack('<3i', grid_meta['file_bbox_max'][1]))
    dims = hi - lo + 1
    grid = np.zeros(dims, np.float32)
    for o0 in leaves:
        vm = mask(r, 512)
        v = values(r, 512, vm, compression, background).reshape(8, 8, 8)
        a = o0 - lo
        s, b = np.maximum(a, 0), np.minimum(a + 8, dims)
        if np.all(b > s):
            grid[s[0]:b[0], s[1]:b[1], s[2]:b[2]] = v[s[0] - a[0]:b[0] - a[0], s[1] - a[1]:b[1] - a[1], s[2] - a[2]:b[2] - a[2]]
    for o, size, v in const:
        a = np.array(o) - lo
        s, b = np.maximum(a, 0), np.minimum(a + size, dims)
        if np.all(b > s):
            grid[s[0]:b[0], s[1]:b[1], s[2]:b[2]] = v
    assert r.p == end_pos, (r.p, end_pos)
    return grid


if __name__ == '__main__':
    import os
    grid = read_volume(sys.argv[1])
    assert grid.shape == (250, 170, 307), grid.shape
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'wdas_cloud', 'wdas_cloud_eighth.r8')
    data = np.clip(np.rint(grid * 255), 0, 255).astype(np.uint8).transpose(2, 1, 0).tobytes()
    open(out, 'wb').write(data)
    print('wrote', out, len(data), 'bytes')
