#!/usr/bin/env python3
"""Decodes the 300x200 PNG png_test.cpp writes, with Python's zlib and struct, and checks every chunk and pixel."""

import struct
import sys
import zlib

WIDTH, HEIGHT = 300, 200


def main(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "PNG signature"
    chunks, at = [], 8
    while at < len(data):
        (length,) = struct.unpack(">I", data[at:at + 4])
        kind, body = data[at + 4:at + 8], data[at + 8:at + 8 + length]
        (crc,) = struct.unpack(">I", data[at + 8 + length:at + 12 + length])
        assert crc == zlib.crc32(kind + body), f"{kind} CRC"
        chunks.append((kind, body))
        at += 12 + length
    assert [kind for kind, _ in chunks] == [b"IHDR", b"IDAT", b"IEND"], "chunk order"
    assert chunks[0][1] == struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0), "IHDR"
    raw = zlib.decompress(chunks[1][1])
    stride = WIDTH * 3 + 1
    assert len(raw) == stride * HEIGHT, "scanline bytes"
    for y in range(HEIGHT):
        row = raw[y * stride:(y + 1) * stride]
        assert row[0] == 0, f"row {y} filter"
        expected = bytes(v for x in range(WIDTH) for v in (x & 255, y & 255, x * y & 255))
        assert row[1:] == expected, f"row {y} pixels"
    print(f"PASS {path}: {WIDTH}x{HEIGHT} decodes with zlib, every CRC and pixel matches")


if __name__ == "__main__":
    main(sys.argv[1])
