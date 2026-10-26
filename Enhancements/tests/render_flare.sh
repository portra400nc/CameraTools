#!/bin/sh
# Draws the lens flare over a frame EnhancementsSaveFrame saved, or a plain picture, into a PNG (tests/render_flare.cpp
# says the keys). Builds render_flare with the add-on's core into $RENDER_FLARE_BIN
# (default: render_flare in $TMPDIR) when a source is newer than it, and recompresses the PNG with Python's zlib, since the
# add-on's encoder stores its pixels uncompressed.
#
#     tests/render_flare.sh <frame folder | black | gray> <out.png> sun=<u>,<v> [key=value ...]
set -e
here=$(cd "$(dirname "$0")" && pwd)
bin=${RENDER_FLARE_BIN:-${TMPDIR:-/tmp}/render_flare}
if [ ! -x "$bin" ] || [ -n "$(find "$here/render_flare.cpp" "$here/../core" "$here/../shaders" "$here/../enhancements.h" -newer "$bin" 2>/dev/null | head -1)" ]; then
    c++ -std=c++17 -O2 -pthread -Wall -Wextra -Werror -I "$here/.." "$here/render_flare.cpp" "$here"/../core/*.cpp -o "$bin"
fi
"$bin" "$@"
    python3 -c '
import struct, sys, zlib
path = sys.argv[1]
data = open(path, "rb").read()
chunks, at = [], 8
while at < len(data):
    (length,) = struct.unpack(">I", data[at:at + 4])
    chunks.append((data[at + 4:at + 8], data[at + 8:at + 8 + length]))
    at += 12 + length
out = data[:8]
for kind, body in chunks:
    if kind == b"IDAT":
        body = zlib.compress(zlib.decompress(body), 9)
    out += struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
open(path, "wb").write(out)
' "$2"
