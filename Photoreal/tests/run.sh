#!/bin/sh
# Builds and runs the Photoreal core's tests, which replay recorded census frames through the frame recipe, with any
# C++17 compiler, then decodes the PNG encoder's output with Python's zlib, checks the synthetic scene folder
# scene_files_test writes with check_scene.py, which needs numpy, and the synthetic traced frame trace_test writes with
# trace_reference and check_trace.py. No game, no ReShade, no D3D11. When it finds build
# 199's whole deduplicated scene (PHOTOREAL_TRACE_DATA, or .work/photoreal/tracer/scene-194351-tris.bin in a folder
# above this one), it also runs check_bvh.sh on it.
set -e
cd "$(dirname "$0")"
out=$(mktemp -d)
for test in *_test.cpp; do
    c++ -std=c++17 -Wall -Wextra -Werror -I .. "$test" fixture.cpp ../core/*.cpp -o "$out/${test%.cpp}"
    "$out/${test%.cpp}" "$out"
done
python3 check_png.py "$out/roundtrip.png"
python3 check_scene.py "$out/scene-synthetic"
# The synthetic traced frame trace_test wrote: traced again by the reference tracer, which check_trace.py must find equal,
# and a copy with a block of sun samples flipped, which it must refuse.
c++ -std=c++17 -O2 -pthread -Wall -Wextra -Werror -I .. trace_reference.cpp ../core/*.cpp -o "$out/trace_reference"
"$out/trace_reference" "$out/trace-synthetic"
python3 check_trace.py "$out/trace-synthetic"
"$out/trace_reference" "$out/trace-synthetic-broken" > "$out/broken.txt"
if python3 check_trace.py "$out/trace-synthetic-broken" >> "$out/broken.txt" 2>&1; then
    echo "FAIL check_trace.py passed a trace whose sun samples differ from the reference's"
    exit 1
fi
echo "PASS check_trace.py refuses a trace whose sun samples differ from the reference's"
data=${PHOTOREAL_TRACE_DATA:-}
if [ -z "$data" ]; then
    dir=$(pwd)
    while [ "$dir" != "/" ]; do
        if [ -f "$dir/.work/photoreal/tracer/scene-194351-tris.bin" ]; then
            data="$dir/.work/photoreal/tracer/scene-194351-tris.bin"
            break
        fi
        dir=$(dirname "$dir")
    done
fi
if [ -n "$data" ] && [ -f "$data" ]; then
    sh check_bvh.sh "$data"
else
    echo "SKIP check_bvh.sh: no scene-194351-tris.bin (set PHOTOREAL_TRACE_DATA to run it)"
fi
