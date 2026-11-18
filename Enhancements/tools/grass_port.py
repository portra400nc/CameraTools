#!/usr/bin/env python3
"""Converts Skyrim SE grass meshes (NIF 20.2.0.7, BSTriShape) into the Enhancements add-on's grass port format, for
drawing in place of Genshin's grass with Genshin's own grass shaders.

  grass_port.py info <file.nif>...                 the shapes, their vertex attributes, size and textures
  grass_port.py mesh <file.nif> <out.gpm> [--hybrid] [--copies N]  one clump (N turned copies of it) in Genshin's grass vertex layout; --hybrid for a texture
                                                   that stacks its color over its normals (Cathedral's Hybrid ones)
  grass_port.py game <vb0.bin> <ib.bin> <out.gpm> <copies>   the game's own clump, dumped, copies times over
  grass_port.py plain <out.dds> <gray>             a 4 x 4 opaque gray texture for a clump with no texture
  grass_port.py texture <in.dds> <out.dds> <width> the DDS from its first mip no wider than width, with no re-encoding

The output format (.gpm, little endian): "GPM2", vertex count u32, index count u32, then the vertices, 44 bytes each in
the layout Genshin's meadow grass input layout reads (position float3, normal float3, TEXCOORD0 float2 the blade's root x
and z, TEXCOORD1 float2 the texture coordinate, 4 bytes of padding), then the indices as u16. Positions are in meters, y up.
"""
import struct
import sys

import numpy as np

SKYRIM_UNIT = 1 / 70.0          # meters


class Reader:
    def __init__(self, data, at=0):
        self.data, self.at = data, at

    def take(self, fmt):
        values = struct.unpack_from('<' + fmt, self.data, self.at)
        self.at += struct.calcsize('<' + fmt)
        return values if len(values) > 1 else values[0]

    def bytes(self, n):
        b = self.data[self.at:self.at + n]
        self.at += n
        return b


def parse_nif(path):
    data = open(path, 'rb').read()
    line_end = data.index(b'\n')
    r = Reader(data, line_end + 1)
    version, endian, user_version, num_blocks = r.take('I'), r.take('B'), r.take('I'), r.take('I')
    bs_version = r.take('I')
    for _ in range(3):              # author, process script, export script
        r.bytes(r.take('B'))
    if bs_version >= 130:
        r.bytes(r.take('B'))
    types = [r.bytes(r.take('I')).decode() for _ in range(r.take('H'))]
    block_types = [types[r.take('H')] for _ in range(num_blocks)]
    sizes = [r.take('I') for _ in range(num_blocks)]
    num_strings, _max = r.take('I'), r.take('I')
    strings = [r.bytes(r.take('I')).decode(errors='replace') for _ in range(num_strings)]
    r.bytes(4 * r.take('I'))        # groups
    blocks = []
    for kind, size in zip(block_types, sizes):
        blocks.append((kind, data[r.at:r.at + size]))
        r.at += size
    return bs_version, blocks, strings


def av_object(r):
    """NiAVObject's fields as SSE stores them: name, extra data, controller, flags, transform, collision."""
    r.take('i')
    r.bytes(4 * r.take('I'))
    r.take('i')
    r.take('I')
    translation = np.array(r.take('3f'))
    rotation = np.array(r.take('9f')).reshape(3, 3)
    scale = r.take('f')
    r.take('i')
    return translation, rotation, scale


def half3(r):
    return np.frombuffer(r.bytes(6), np.float16).astype(np.float64)


def parse_tri_shape(block):
    r = Reader(block)
    translation, rotation, scale = av_object(r)
    r.take('4f')                    # bounding sphere
    _skin, shader, _alpha = r.take('3i')
    desc = r.take('Q')
    num_triangles = r.take('H')     # SSE keeps it in 16 bits
    num_vertices = r.take('H')
    data_size = r.take('I')
    flags = (desc >> 44) & 0xFFF
    stride = (desc & 0xF) * 4
    offset = lambda shift: ((desc >> shift) & 0xF) * 4
    uv_at, normal_at, color_at = offset(8), offset(16), offset(24)
    positions, uvs, normals = [], [], []
    for i in range(num_vertices):
        v = block[r.at + i * stride:r.at + (i + 1) * stride]
        positions.append(struct.unpack_from('<3f', v, 0))
        uvs.append(np.frombuffer(v[uv_at:uv_at + 4], np.float16).astype(np.float64) if flags & 0x2 else (0, 0))
        normals.append(np.frombuffer(v[normal_at:normal_at + 3], np.uint8).astype(np.float64) / 127.5 - 1 if flags & 0x8 else (0, 0, 1))
    r.at += num_vertices * stride
    triangles = np.array(r.take(f'{3 * num_triangles}H'), np.int64).reshape(-1, 3) if num_triangles else np.zeros((0, 3), np.int64)
    positions = (np.array(positions) * scale) @ rotation.T + translation
    normals = np.array(normals) @ rotation.T
    return dict(positions=positions, uvs=np.array(uvs), normals=normals, triangles=triangles, shader=shader, flags=flags, stride=stride)


def texture_of(blocks, strings, shader_ref):
    kind, block = blocks[shader_ref]
    if kind != 'BSLightingShaderProperty':
        return None
    r = Reader(block)
    r.take('I')                     # shader type
    r.take('i')                     # name
    r.bytes(4 * r.take('I'))        # extra data
    r.take('i')                     # controller
    r.take('Q')                     # shader flags 1 and 2
    r.take('4f')                    # uv offset and scale
    texture_set = r.take('i')
    kind, block = blocks[texture_set]
    t = Reader(block)
    return [t.bytes(t.take('I')).decode(errors='replace') for _ in range(t.take('I'))][0]


def shapes(path):
    bs_version, blocks, strings = parse_nif(path)
    out = []
    for kind, block in blocks:
        if kind == 'BSTriShape':
            s = parse_tri_shape(block)
            s['texture'] = texture_of(blocks, strings, s['shader'])
            out.append(s)
    return out


def info(paths):
    for path in paths:
        for s in shapes(path):
            p = s['positions'] * SKYRIM_UNIT
            print(f"{path.split('/')[-1]}: {len(p)} vertices, {len(s['triangles'])} triangles, flags {s['flags']:#x}, "
                  f"x {p[:, 0].min():.2f}..{p[:, 0].max():.2f} y {p[:, 1].min():.2f}..{p[:, 1].max():.2f} z {p[:, 2].min():.2f}..{p[:, 2].max():.2f} m, "
                  f"texture {s['texture']}")


def components(count, triangles):
    parent = list(range(count))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a
    for a, b, c in triangles:
        for x, y in ((a, b), (b, c)):
            rx, ry = find(x), find(y)
            if rx != ry:
                parent[rx] = ry
    return [find(i) for i in range(count)]


def multiply(p, n, extra, tris, copies, roots=None):
    """copies of a clump in one mesh, each turned by the golden angle, moved up to a few decimeters and a little smaller:
    denser grass at no more draws. roots, the blades' root x and z, turn and move with them."""
    if copies <= 1:
        return (p, n, extra, tris) if roots is None else (p, n, extra, tris, roots)
    ps, ns, es, ts, rs = [], [], [], [], []
    for k in range(copies):
        angle = k * 2.39996323
        c, s = np.cos(angle), np.sin(angle)
        turn = np.array([[c, 0, -s], [0, 1, 0], [s, 0, c]])
        shift = np.array([np.cos(angle * 1.7), 0, np.sin(angle * 1.7)]) * (0.35 * np.sqrt(k / copies))
        scale = 1 - 0.25 * k / copies
        ps.append((p * scale) @ turn.T + shift)
        ns.append(n @ turn.T)
        es.append(extra)
        ts.append(tris + k * len(p))
        if roots is not None:
            r3 = np.stack([roots[:, 0], np.zeros(len(roots)), roots[:, 1]], 1)
            r3 = (r3 * scale) @ turn.T + shift
            rs.append(r3[:, [0, 2]])
    out = (np.concatenate(ps), np.concatenate(ns), np.concatenate(es), np.concatenate(ts))
    return out if roots is None else out + (np.concatenate(rs),)


def game(vb, ib, out, copies):
    """The game's own grass clump, as tools dumped it (44-byte vertices: position, normal, the blade's root x and z, its
    wind phase and height weight), copies times over."""
    data = open(vb, 'rb').read()
    v = np.frombuffer(data, np.uint8).reshape(-1, 44)
    f = v[:, :40].copy().view(np.float32)
    tris = np.frombuffer(open(ib, 'rb').read(), np.uint16).reshape(-1, 3).astype(np.int64)
    p, n, extra, tris, roots = multiply(f[:, 0:3], f[:, 3:6], f[:, 8:10], tris, copies, roots=f[:, 6:8])
    with open(out, 'wb') as o:
        o.write(b'GPM2' + struct.pack('<II', len(p), tris.size))
        for i in range(len(p)):
            o.write(struct.pack('<3f3f2f2fI', *p[i], *n[i], *roots[i], *extra[i], 0))
        o.write(tris.astype(np.uint16).tobytes())
    print(f"{out}: {len(p)} vertices, {len(tris)} triangles, {copies} copies of the game's clump")


def plain(out, srgb):
    """A 4 x 4 opaque gray texture, uncompressed, for a clump with no texture of its own: its detail reads as 1."""
    header = bytearray(148)
    struct.pack_into('<4sIIIIIII', header, 0, b'DDS ', 124, 0x1007, 4, 4, 16, 0, 1)
    struct.pack_into('<II4s', header, 76, 32, 4, b'DX10')
    struct.pack_into('<IIIII', header, 128, 29, 3, 0, 1, 0)
    with open(out, 'wb') as o:
        o.write(header + bytes([srgb, srgb, srgb, 255]) * 16)
    print(f"{out}: 4 x 4 gray {srgb}")


def mesh(path, out, hybrid=False, copies=1):
    positions, normals, uvs, triangles = [], [], [], []
    base = 0
    for s in shapes(path):
        positions.append(s['positions'])
        normals.append(s['normals'])
        uvs.append(s['uvs'])
        triangles.append(s['triangles'] + base)
        base += len(s['positions'])
    # Skyrim is z up: Genshin's y is Skyrim's z.
    p = np.concatenate(positions) * SKYRIM_UNIT
    p = p[:, [0, 2, 1]]
    n = np.concatenate(normals)[:, [0, 2, 1]]
    uv = np.concatenate(uvs)
    # Cathedral's hybrid grass textures stack the color over the normals, and its clumps reach into both halves: the
    # color is always in the top half, at the same place.
    if hybrid:
        uv[:, 1] = np.where(uv[:, 1] > 0.5, uv[:, 1] - 0.5, uv[:, 1])
    tris = np.concatenate(triangles)
    tris = tris[:, [0, 2, 1]]       # the axis swap mirrors the winding
    p, n, uv, tris = multiply(p, n, uv, tris, copies)
    root_of = components(len(p), tris)
    roots = {}
    for i, r in enumerate(root_of):
        if r not in roots or p[i, 1] < p[roots[r], 1]:
            roots[r] = i
    vertices = bytearray()
    for i in range(len(p)):
        root = p[roots[root_of[i]]]
        normal = n[i] / max(np.linalg.norm(n[i]), 1e-6)
        vertices += struct.pack('<3f3f2f2fI', *p[i], *normal, root[0], root[2], *uv[i], 0)
    with open(out, 'wb') as f:
        f.write(b'GPM2' + struct.pack('<II', len(p), tris.size))
        f.write(vertices)
        f.write(tris.astype(np.uint16).tobytes())
    print(f"{out}: {len(p)} vertices, {len(tris)} triangles, {len(roots)} blades, {p[:, 1].max() - p[:, 1].min():.2f} m tall, "
          f"{p[:, 0].max() - p[:, 0].min():.2f} x {p[:, 2].max() - p[:, 2].min():.2f} m across")


BLOCK_BYTES = {98: 16, 99: 16, 71: 8, 72: 8, 77: 16, 78: 16}   # BC7, BC1, BC3, as DXGI formats


def texture(src, out, width):
    data = open(src, 'rb').read()
    height, full_width = struct.unpack_from('<II', data, 12)
    mips = struct.unpack_from('<I', data, 28)[0]
    fmt = struct.unpack_from('<I', data, 128)
    fmt = fmt[0] if isinstance(fmt, tuple) else fmt
    block = BLOCK_BYTES[fmt]
    at = 148
    w, h, level = full_width, height, 0
    while w > width and level < mips - 1:
        at += max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * block
        w, h, level = max(1, w // 2), max(1, h // 2), level + 1
    header = bytearray(data[:148])
    struct.pack_into('<II', header, 12, h, w)
    struct.pack_into('<I', header, 20, max(1, (w + 3) // 4) * block)
    struct.pack_into('<I', header, 28, mips - level)
    with open(out, 'wb') as f:
        f.write(header + data[at:])
    print(f"{out}: {w} x {h}, {mips - level} mips, from level {level} of {src.split('/')[-1]}")


if __name__ == '__main__':
    command, args = sys.argv[1], sys.argv[2:]
    if command == 'info':
        info(args)
    elif command == 'mesh':
        copies = int(args[args.index('--copies') + 1]) if '--copies' in args else 1
        mesh(args[0], args[1], hybrid='--hybrid' in args, copies=copies)
    elif command == 'game':
        game(args[0], args[1], args[2], int(args[3]))
    elif command == 'plain':
        plain(args[0], int(args[1]))
    elif command == 'texture':
        texture(args[0], args[1], int(args[2]))
