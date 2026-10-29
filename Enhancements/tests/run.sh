#!/bin/sh
# Builds and runs the Enhancements core's tests, which replay recorded census frames through the frame recipe, with any
# C++17 compiler, then decodes the PNG encoder's output with Python's zlib and the DNG writer's with macOS's sips, renders the depth of field harness's synthetic
# frame, checks the synthetic scene folder scene_files_test writes with check_scene.py, which needs numpy, and the synthetic traced frame trace_test writes with
# trace_reference and check_trace.py, and its gathering save and three broken copies of it. No game, no ReShade, no D3D11. With vkd3d-compiler (ENHANCEMENTS_VKD3D_COMPILER, or
# the melonloader workspace's .work/build/reshade/vkd3d/vkd3d-compiler in a folder above this one), check_kernel_size.py
# holds the compiled trace kernel to build 203's size. When it finds build 199's whole deduplicated scene
# (ENHANCEMENTS_TRACE_DATA, or .work/photoreal/tracer/scene-194351-tris.bin in a folder above this one), it also runs
# check_bvh.sh on it.
set -e
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
for test in *_test.cpp; do
    c++ -std=c++17 -Wall -Wextra -Werror -I .. "$test" fixture.cpp ../core/*.cpp -o "$out/${test%.cpp}"
    "$out/${test%.cpp}" "$out"
done
python3 check_png.py "$out/roundtrip.png"
# macOS's own raw decoder (sips, through ImageIO) must open the DNG writer's 320x200 ramp at its size.
png_size='import struct, sys; d = open(sys.argv[1], "rb").read(); sys.exit(0 if d[12:24] == b"IHDR" + struct.pack(">II", int(sys.argv[2]), int(sys.argv[3])) else 1)'
if command -v sips > /dev/null; then
    sips -s format png "$out/ramp.dng" --out "$out/ramp.png" > /dev/null 2>&1 || true
    python3 -c "$png_size" "$out/ramp.png" 320 200 2> /dev/null || { echo "FAIL sips does not decode the DNG writer's ramp into a 320x200 PNG"; exit 1; }
    echo "PASS sips decodes the DNG writer's ramp into a 320x200 PNG"
else
    echo "SKIP the DNG's decode by sips: no sips"
fi
# The depth of field's harness: the synthetic frame folder written, and rendered at spherical aberration 1 into a PNG of
# its size.
RENDER_DOF_BIN="$out/render_dof" sh render_dof.sh --synthetic "$out/frame-synthetic" > /dev/null
RENDER_DOF_BIN="$out/render_dof" sh render_dof.sh "$out/frame-synthetic" "$out/dof.png" sa=1 > /dev/null
python3 -c "import struct, sys; d = open(sys.argv[1], 'rb').read(); sys.exit(0 if d[12:24] == b'IHDR' + struct.pack('>II', 1280, 800) else 1)" "$out/dof.png"
echo "PASS render_dof renders the synthetic frame into a 1280x800 PNG"
# The lens flare's harness: the flare over a plain picture, with the sun just past its top right corner.
RENDER_FLARE_BIN="$out/render_flare" sh render_flare.sh black "$out/flare.png" sun=1.05,-0.05 > /dev/null
python3 -c "import struct, sys; d = open(sys.argv[1], 'rb').read(); sys.exit(0 if d[12:24] == b'IHDR' + struct.pack('>II', 1280, 800) else 1)" "$out/flare.png"
echo "PASS render_flare draws the flare into a 1280x800 PNG"
# A blurred plane 1 m away over a plane in focus at 10 m or a little in front of it: its edge must spread over what lies
# behind with a disc's edge spread, within 5% of its 10-90% width, at 800 and 2160 rows in both places. Over a background
# 8 m away, at 2160 rows, f/1 and max blur 0.08, it spread 4.0 px of 139.8 while a pixel's own surface in front of focus
# went into one near field with the edges in front of it. Its profile must also fall all the way: filtered samples of the
# background beside the silhouette brought the foreground's light in twice, a line 1.8% of the step brighter.
"$out/render_dof" --near-edge-check 0.05 > "$out/near-edge.txt" || { cat "$out/near-edge.txt"; echo "FAIL a blurred foreground's edge does not spread over the background like a disc's"; exit 1; }
echo "PASS a blurred foreground's edge spreads over a background in focus or in front of it like a disc's, with no line along it"
# The workspace's saved frames (.work/dof/frames in a folder above this one). The night plaza, frame-20261024-145312:
# rendered at chromatic 1 and f/1.4 focused on the subject and just behind it, with a long lens, every half-float target
# must stay finite as R16F stores it (render_dof exits 3 otherwise). The portrait through a 202 mm lens,
# frame-20261024-195721: at the user's settings (f/2.8, spherical aberration 0.6, cat's eye 0.6, bokeh contrast 0.5,
# autofocus) and at the chromatic sweep's worst case before the fix (300 mm focused at 2.713 m), chromatic aberration must
# drop no channel and leave no tile-sized hue jump; and through a 300 mm lens focused on the face at 2.606 m, what the bokeh
# contrast changes may step by at most 2 levels across a gather tile's edge (it stepped by 11.6 when the post-filter took its
# span from each tile's own radius).
dir=$(pwd)
frames=
while [ "$dir" != "/" ]; do
    if [ -d "$dir/.work/dof/frames" ]; then
        frames="$dir/.work/dof/frames"
        break
    fi
    dir=$(dirname "$dir")
done
plaza=$frames/frame-20261024-145312
if [ -n "$frames" ] && [ -f "$plaza/frame.tsv" ]; then
    RENDER_DOF_BIN="$out/render_dof" sh render_dof.sh "$plaza" "$out/plaza.png" f=1.4 chromatic=1 crop=0,0,8,8 > /dev/null
    RENDER_DOF_BIN="$out/render_dof" sh render_dof.sh "$plaza" "$out/plaza.png" f=1.4 chromatic=1 sensor=72 focus=2.25 max_blur=0.05 crop=0,0,8,8 > /dev/null
    echo "PASS render_dof keeps every half-float target finite on the night plaza at chromatic 1"
else
    echo "SKIP the night plaza's chromatic render: no .work/dof/frames/frame-20261024-145312"
fi
portrait=$frames/frame-20261024-195721
if [ -n "$frames" ] && [ -f "$portrait/frame.tsv" ]; then
    "$out/render_dof" --chromatic-check "$portrait" 0 sensor=22.95 > "$out/portrait.txt" || { cat "$out/portrait.txt"; echo "FAIL chromatic aberration broke the portrait through a 202 mm lens"; exit 1; }
    "$out/render_dof" --chromatic-check "$portrait" 0 mm=300 focus=2.713 > "$out/portrait-300.txt" || { cat "$out/portrait-300.txt"; echo "FAIL chromatic aberration broke the portrait through a 300 mm lens"; exit 1; }
    echo "PASS chromatic aberration drops no channel and leaves no tile-sized hue jump on the portrait through 202 and 300 mm lenses"
    "$out/render_dof" --seam-check "$portrait" 2 mm=300 focus=2.606 chromatic=0 > "$out/portrait-seam.txt" || { cat "$out/portrait-seam.txt"; echo "FAIL the bokeh contrast steps on the gather tiles' edges in the portrait through a 300 mm lens"; exit 1; }
    echo "PASS the bokeh contrast leaves no step on the gather tiles' edges in the portrait through a 300 mm lens"
else
    echo "SKIP the portrait's chromatic and seam checks: no .work/dof/frames/frame-20261024-195721"
fi
# The harbor, frame-20261024-134128, written as a DNG with frame_dng's test EXIF: byte for byte the prototype's compressed
# file (.work/dng/lightroom-test/harbor-exif.dng, the layout the user opened in Lightroom on 2026-10-29), which sips must
# decode at the frame's size.
harbor=$frames/frame-20261024-134128
prototype=${frames%/dof/frames}/dng/lightroom-test/harbor-exif.dng
if [ -n "$frames" ] && [ -f "$harbor/frame.tsv" ] && [ -f "$prototype" ]; then
    c++ -std=c++17 -O2 -Wall -Wextra -Werror -I .. frame_dng.cpp ../core/*.cpp -o "$out/frame_dng"
    "$out/frame_dng" "$harbor" "$out/harbor.dng" exif > /dev/null
    cmp -s "$out/harbor.dng" "$prototype" || { echo "FAIL frame_dng's DNG of the harbor differs from the prototype's compressed file"; exit 1; }
    echo "PASS frame_dng writes the harbor's DNG byte for byte as the prototype's compressed file"
    if command -v sips > /dev/null; then
        sips -s format png "$out/harbor.dng" --out "$out/harbor.png" > /dev/null 2>&1 || true
        python3 -c "$png_size" "$out/harbor.png" 1152 720 2> /dev/null || { echo "FAIL sips does not decode the harbor's DNG into a 1152x720 PNG"; exit 1; }
        echo "PASS sips decodes the harbor's DNG into a 1152x720 PNG"
    fi
    rm -f "$out/harbor.dng" "$out/harbor.png"
else
    echo "SKIP the harbor's DNG: no .work/dof/frames/frame-20261024-134128 or .work/dng/lightroom-test/harbor-exif.dng"
fi
python3 check_kernel_conditions.py ../shaders
vkd3d=${ENHANCEMENTS_VKD3D_COMPILER:-}
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
    echo "SKIP check_kernel_size.py: no vkd3d-compiler (set ENHANCEMENTS_VKD3D_COMPILER to run it)"
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
data=${ENHANCEMENTS_TRACE_DATA:-}
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
    echo "SKIP check_bvh.sh: no scene-194351-tris.bin (set ENHANCEMENTS_TRACE_DATA to run it)"
fi
