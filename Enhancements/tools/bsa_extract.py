#!/usr/bin/env python3
"""Lists or extracts a Skyrim SE archive (BSA version 105), its LZ4-framed files too, with no modules beyond the standard
library: the grass port's tools read mods packed this way.

  bsa_extract.py list <archive.bsa>
  bsa_extract.py extract <archive.bsa> <folder> [substring]
"""
import os
import struct
import sys


def lz4_block(src, size):
    out = bytearray()
    i = 0
    while i < len(src):
        token = src[i]
        i += 1
        literals = token >> 4
        if literals == 15:
            while True:
                b = src[i]
                i += 1
                literals += b
                if b != 255:
                    break
        out += src[i:i + literals]
        i += literals
        if i >= len(src) or len(out) >= size:
            break
        offset = src[i] | src[i + 1] << 8
        i += 2
        length = token & 15
        if length == 15:
            while True:
                b = src[i]
                i += 1
                length += b
                if b != 255:
                    break
        length += 4
        start = len(out) - offset
        for k in range(length):
            out.append(out[start + k])
    return bytes(out)


def lz4_frame(data, size):
    if struct.unpack_from('<I', data, 0)[0] != 0x184D2204:
        raise ValueError('not an LZ4 frame')
    flags = data[4]
    at = 7 + (8 if flags & 0x08 else 0) + (4 if flags & 0x01 else 0)
    out = bytearray()
    while True:
        block = struct.unpack_from('<I', data, at)[0]
        at += 4
        if block == 0:
            break
        raw = block & 0x80000000
        block &= 0x7FFFFFFF
        chunk = data[at:at + block]
        out += chunk if raw else lz4_block(chunk, size - len(out))
        at += block + (4 if flags & 0x10 else 0)
    return bytes(out)


def entries(path):
    data = open(path, 'rb').read()
    magic, version, offset, flags, folders, files, _, _, _ = struct.unpack_from('<4sIIIIIIII', data, 0)
    if magic != b'BSA\0' or version != 105:
        raise ValueError(f'{path}: not an SSE archive')
    compressed, embedded = bool(flags & 0x4), bool(flags & 0x100)
    at = offset
    counts = []
    for _ in range(folders):
        _, count, _, _ = struct.unpack_from('<QIIQ', data, at)
        counts.append(count)
        at += 24
    records = []
    for count in counts:
        n = data[at]
        folder = data[at + 1:at + n].decode(errors='replace')
        at += 1 + n
        for _ in range(count):
            _, size, where = struct.unpack_from('<QII', data, at)
            records.append((folder, size, where))
            at += 16
    names = data[at:].split(b'\0')
    for (folder, size, where), name in zip(records, names):
        packed = compressed != bool(size & 0x40000000)
        size &= 0x3FFFFFFF
        start = where
        if embedded:
            start += 1 + data[start]
            size -= 1 + data[where]
        yield os.path.join(folder, name.decode(errors='replace')).replace('\\', '/'), data, start, size, packed


def read(data, start, size, packed):
    if not packed:
        return data[start:start + size]
    original = struct.unpack_from('<I', data, start)[0]
    return lz4_frame(data[start + 4:start + size], original)


if __name__ == '__main__':
    command, archive = sys.argv[1], sys.argv[2]
    if command == 'list':
        for name, _, _, size, packed in entries(archive):
            print(f'{size:>10} {"lz4" if packed else "   "} {name}')
    else:
        folder, match = sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else ''
        for name, data, start, size, packed in entries(archive):
            if match.lower() in name.lower():
                target = os.path.join(folder, name)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                open(target, 'wb').write(read(data, start, size, packed))
                print(target)
