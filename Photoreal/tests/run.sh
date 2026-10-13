#!/bin/sh
# Builds and runs the Photoreal core's tests, which replay recorded census frames through the frame recipe, with any
# C++17 compiler, then decodes the PNG encoder's output with Python's zlib, checks the synthetic scene folder
# scene_files_test writes with check_scene.py, which needs numpy, and the synthetic traced frame trace_test writes with
# trace_reference and check_trace.py, and its gathering save and three broken copies of it. No game, no ReShade, no D3D11. With vkd3d-compiler (PHOTOREAL_VKD3D_COMPILER, or
# the melonloader workspace's .work/build/reshade/vkd3d/vkd3d-compiler in a folder above this one), check_kernel_size.py
# holds the compiled trace kernel to build 203's size. When it finds build 199's whole deduplicated scene
# (PHOTOREAL_TRACE_DATA, or .work/photoreal/tracer/scene-194351-tris.bin in a folder above this one), it also runs
# check_bvh.sh on it.
set -e
cd "$(dirname "$0")"
out=$(mktemp -d)
for test in *_test.cpp; do
    c++ -std=c++17 -Wall -Wextra -Werror -I .. "$test" fixture.cpp ../core/*.cpp -o "$out/${test%.cpp}"
    "$out/${test%.cpp}" "$out"
done
python3 check_png.py "$out/roundtrip.png"
python3 check_kernel_conditions.py ../shaders
vkd3d=${PHOTOREAL_VKD3D_COMPILER:-}
if [ -z "$vkd3d" ]; then
    dir=$(pwd)
    while [ "$dir" != "/" ]; do
        if [ -x "$dir/.work/build/reshade/vkd3d/vkd3d-compiler" ]; then
            vkd3d="$dir/.work/build/reshade/vkd3d/vkd3d-compiler"
            break
        fi
        dir=$(dirname "$dir")
    done
fi
if [ -n "$vkd3d" ]; then
    python3 check_kernel_size.py "$vkd3d" ../shaders
else
    echo "SKIP check_kernel_size.py: no vkd3d-compiler (set PHOTOREAL_VKD3D_COMPILER to run it)"
fi
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
# The synthetic gathering save: traced again as the add-on traced it, which check_trace.py must find equal, and three
# broken copies it must refuse: a block's bounce doubled, a block's misses dropped, and the sky gathered turned a quarter.
"$out/trace_reference" "$out/trace-synthetic-gather"
python3 check_trace.py "$out/trace-synthetic-gather"
# The same light gathered from each pixel's next 8 samples: other rays, as the Deck's are where its rounding turns one, which
# check_trace.py must find in agreement, though pixels have hits, or hits the sun lights, on one side only.
"$out/trace_reference" "$out/trace-synthetic-gather-other"
python3 check_trace.py "$out/trace-synthetic-gather-other"
for broken in bounce-doubled misses-dropped sky-turned; do
    "$out/trace_reference" "$out/trace-synthetic-gather-$broken" > "$out/broken-$broken.txt"
    if python3 check_trace.py "$out/trace-synthetic-gather-$broken" >> "$out/broken-$broken.txt" 2>&1; then
        echo "FAIL check_trace.py passed a gathering trace with its $broken"
        exit 1
    fi
    echo "PASS check_trace.py refuses a gathering trace with its $broken"
done
# The synthetic light capture light_test wrote: its light baked again to the bit and reported on, and a copy whose view
# face's albedo was brightened, which bake_light must find off.
sh check_light.sh "$out/scene-light-synthetic" "$out/trace-light-synthetic"
cp -R "$out/scene-light-synthetic" "$out/scene-light-broken"
python3 -c "import sys; p = sys.argv[1]; b = bytearray(open(p, 'rb').read()); b[0::4] = bytes(min(v + 40, 255) for v in b[0::4]); open(p, 'wb').write(b)" \
    "$out/scene-light-broken/face-6-albedo.rgba8"
if sh check_light.sh "$out/scene-light-broken" > "$out/broken-light.txt" 2>&1; then
    echo "FAIL bake_light passed a capture whose view face's albedo was changed"
    exit 1
fi
echo "PASS bake_light refuses a capture whose view face's albedo differs from what made trace-light.bin"
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
