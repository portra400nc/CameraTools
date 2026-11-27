#!/bin/sh
# Builds and runs the Enhancements core's tests, which replay recorded census frames through the frame recipe, with any
# C++17 compiler, then decodes the PNG encoder's output with Python's zlib and the DNG writer's with macOS's sips, renders the depth of field harness's synthetic
# frame, checks the synthetic scene folder scene_files_test writes with check_scene.py, which needs numpy, and the synthetic traced frame trace_test writes with
# trace_reference and check_trace.py, and its gathering save and three broken copies of it. No game, no ReShade, no D3D11. With vkd3d-compiler (ENHANCEMENTS_VKD3D_COMPILER, or
# the melonloader workspace's .work/build/reshade/vkd3d/vkd3d-compiler in a folder above this one), check_kernel_size.py
# holds the compiled trace kernel to build 203's size. When it finds build 199's whole deduplicated scene
# (ENHANCEMENTS_TRACE_DATA, or .work/photoreal/tracer/scene-194351-tris.bin in a folder above this one), it also runs
# check_bvh.sh on it. The core compiles once, optimized, and the tests run side by side. The checks over saved frames and
# the scene run only when their tool's code, arguments or inputs changed since their last pass; FULL=1 runs them all.
set -e
# A step that stops the run, as a test that does not compile, says so: the run's callers look for FAIL.
trap 'echo "FAIL tests/run.sh stopped at line $LINENO (a compile error or a missing file above)"' ERR
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
jobs=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
# The core compiles once, optimized and in parallel, and every test and tool links it: each used to compile all of it again,
# one after another, and dof_test ran 313 s unoptimized against 13 s optimized.
flags="-std=c++17 -O2 -pthread -Wall -Wextra -Werror -I .."
cache=${ENHANCEMENTS_TEST_CACHE:-${TMPDIR:-/tmp}/enhancements-test-cache}
mkdir -p "$cache/objects"
# Each object is kept by a hash of its source, every header of ours, the shaders the core compiles as C++ and the flags,
# and compiles again only when one of them changed: compiling the core and the tests anew took 21 s of every run.
headers=$(cat ../*.h ../core/*.h ../shaders/*.hlsli | shasum | cut -c1-40)
mkdir "$out/core"
ls ../core/*.cpp fixture.cpp *_test.cpp | xargs -P "$jobs" -I {} sh -c '
    key=$( { echo "$1 $4"; cat "$2"; } | shasum | cut -c1-40)
    [ -f "$5/objects/$key.o" ] || c++ $1 -c "$2" -o "$5/objects/$key.o.part" && mv -f "$5/objects/$key.o.part" "$5/objects/$key.o" 2>/dev/null
    cp "$5/objects/$key.o" "$3/core/$(basename "$2" .cpp).o"' _ "$flags" {} "$out" "$headers" "$cache"
mv "$out/core/fixture.o" "$out/fixture.o"
mkdir "$out/tests"
for test in *_test.cpp; do mv "$out/core/${test%.cpp}.o" "$out/tests/"; done
# The tests and the tools link only the code they reach, so a binary changes only when that code does.
strip=
[ "$(uname)" = Darwin ] && strip=-Wl,-dead_strip
ls *_test.cpp | sed 's/\.cpp$//' | xargs -P "$jobs" -I {} sh -c 'c++ $1 $4 "$3/tests/$2.o" "$3/fixture.o" "$3"/core/*.o -o "$3/$2"' _ "$flags" {} "$out" "$strip"
for tool in render_dof render_flare render_sky trace_reference bake_light check_bvh frame_dng; do echo "$tool"; done \
    | xargs -P "$jobs" -I {} sh -c 'c++ $1 $4 "$2.cpp" "$3"/core/*.o -o "$3/$2"' _ "$flags" {} "$out" "$strip"
# A check that renders saved frames or a whole scene again runs only when its tool's binary, its arguments or its inputs
# differ from those of its last pass, which ENHANCEMENTS_TEST_CACHE records. FULL=1 runs them all.
key=
unchanged=0
fresh() {
    key=$( { shasum < "$1"; shift; for input in "$@"; do if [ -e "$input" ]; then ls -lRT "$input"; else echo "$input"; fi; done; } | shasum | cut -c1-40)
    if [ "${FULL:-0}" != 1 ] && [ -f "$cache/$key" ]; then
        unchanged=$((unchanged + 1))
        return 1
    fi
}
passed() { touch "$cache/$key"; }
# The tests run side by side, each into its own log, which prints in order. A test whose binary passed before prints that
# pass's log again without running (dof_test alone ran 33 s), but those that write the files the checks below read; FULL=1
# runs them all.
writers=" dng_test png_test trace_files_test light_test scene_files_test trace_test "
ls *_test.cpp | sed 's/\.cpp$//' | xargs -P "$jobs" -I {} sh -c '
    key=$(shasum < "$2/$1" | cut -c1-40)
    case "$5" in *" $1 "*) ;; *) if [ "${4:-0}" != 1 ] && [ -f "$3/test-$key.log" ]; then cp "$3/test-$key.log" "$2/$1.log"; echo 0 > "$2/$1.status"; exit 0; fi ;; esac
    "$2/$1" "$2" > "$2/$1.log" 2>&1; status=$?; echo $status > "$2/$1.status"
    if [ $status = 0 ]; then cp "$2/$1.log" "$3/test-$key.log"; fi' _ {} "$out" "$cache" "${FULL:-0}" "$writers"
failed=0
for test in *_test.cpp; do
    cat "$out/${test%.cpp}.log"
    [ "$(cat "$out/${test%.cpp}.status")" = 0 ] || failed=1
done
[ "$failed" = 0 ] || { echo "FAIL a test above failed"; exit 1; }
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
# background beside the silhouette brought the foreground's light in twice, a line 1.8% of the step brighter. With the
# edge anti-aliased into a pixel of the background in focus, that pixel kept the foreground's share sharp under the blur,
# a line 13% of the step brighter tracing the outline; and the background the plane hides took that pixel's blend for
# the nearest visible background, so the profile fell 4.2 times a disc's steepest in one step along the depth's outline.
# A farther plane in front of focus beside the nearer one must blur its own edge with its own disc: blurred with the
# nearer plane's, as every surface in front of focus was by the largest disc around, it spread 3.9 times as wide.
if fresh "$out/render_dof" near-edge 0.05; then
    "$out/render_dof" --near-edge-check 0.05 > "$out/near-edge.txt" || { cat "$out/near-edge.txt"; echo "FAIL a blurred foreground's edge does not spread over the background like a disc's"; exit 1; }
    passed
fi
echo "PASS a blurred foreground's edge spreads over a background in focus or in front of it like a disc's, with no line or step along it, and a farther one beside it blurs with its own disc"
# A subject in focus over a background far out of focus, its outline anti-aliased as the game's SMAA or a supersampled shot
# leave it: every pixel must come back as it went in, within 1% of the step between the two. The composite took a pixel
# by its depth alone, so an outline pixel half the subject with the background's depth went into the blur, 48% off, and the
# outline followed the depth's stair-steps; and the gather's filtered reads and the prefilter averaged the subject's light
# into the background's blur, a halo up to 6.8% of the step around it.
if fresh "$out/render_dof" far-edge 0.01 0.01; then
    "$out/render_dof" --far-edge-check 0.01 0.01 > "$out/far-edge.txt" || { cat "$out/far-edge.txt"; echo "FAIL a subject in focus loses its anti-aliased outline or spreads a halo over the blurred background"; exit 1; }
    passed
fi
echo "PASS a subject in focus keeps its anti-aliased outline over a blurred background, with no halo"
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
    if fresh "$out/render_dof" "$plaza" render_dof.sh plaza; then
        RENDER_DOF_BIN="$out/render_dof" sh render_dof.sh "$plaza" "$out/plaza.png" f=1.4 chromatic=1 crop=0,0,8,8 > /dev/null
        RENDER_DOF_BIN="$out/render_dof" sh render_dof.sh "$plaza" "$out/plaza.png" f=1.4 chromatic=1 sensor=72 focus=2.25 max_blur=0.05 crop=0,0,8,8 > /dev/null
        passed
    fi
    echo "PASS render_dof keeps every half-float target finite on the night plaza at chromatic 1"
else
    echo "SKIP the night plaza's chromatic render: no .work/dof/frames/frame-20261024-145312"
fi
portrait=$frames/frame-20261024-195721
if [ -n "$frames" ] && [ -f "$portrait/frame.tsv" ] && ! fresh "$out/render_dof" "$portrait" portrait; then
    echo "PASS chromatic aberration drops no channel and leaves no tile-sized hue jump on the portrait through 202 and 300 mm lenses"
    echo "PASS the bokeh contrast leaves no step on the gather tiles' edges in the portrait through a 300 mm lens"
elif [ -n "$frames" ] && [ -f "$portrait/frame.tsv" ]; then
    "$out/render_dof" --chromatic-check "$portrait" 0 sensor=22.95 > "$out/portrait.txt" || { cat "$out/portrait.txt"; echo "FAIL chromatic aberration broke the portrait through a 202 mm lens"; exit 1; }
    "$out/render_dof" --chromatic-check "$portrait" 0 mm=300 focus=2.713 > "$out/portrait-300.txt" || { cat "$out/portrait-300.txt"; echo "FAIL chromatic aberration broke the portrait through a 300 mm lens"; exit 1; }
    echo "PASS chromatic aberration drops no channel and leaves no tile-sized hue jump on the portrait through 202 and 300 mm lenses"
    "$out/render_dof" --seam-check "$portrait" 2 mm=300 focus=2.606 chromatic=0 > "$out/portrait-seam.txt" || { cat "$out/portrait-seam.txt"; echo "FAIL the bokeh contrast steps on the gather tiles' edges in the portrait through a 300 mm lens"; exit 1; }
    echo "PASS the bokeh contrast leaves no step on the gather tiles' edges in the portrait through a 300 mm lens"
    passed
else
    echo "SKIP the portrait's chromatic and seam checks: no .work/dof/frames/frame-20261024-195721"
fi
# The railing, frame-20261029-143455: posts close to a 51 mm lens at f/1.2 focused on the castle far behind, through the
# display place at 4K (the frame upscaled, its color smooth and its depth stair-stepped) with the user's soap bubble 0.5,
# cat's eye 0.7 and chromatic 1. Inside the second post, neighbors may differ by at most 20 levels: the fill of what the post
# hides was cut by the cat's eye, and the post's own color showed through its blur in a step of 57 along its outline.
railing=$frames/frame-20261029-143455
if [ -n "$frames" ] && [ -f "$railing/frame.tsv" ]; then
    if fresh "$out/render_dof" "$railing" render_dof.sh railing; then
        RENDER_DOF_BIN="$out/render_dof" sh render_dof.sh "$railing" "$out/railing.png" f=1.2 mm=51 focus=1170 max_blur=0.02 sa=0.5 chromatic=1 cat_eye=0.7 \
            scatter=1 gain=4 threshold=1.5 samples=289 roundness=0.1 display=1 upscale=3 crop=2550,1360,400,340 near_steps=10 most_steps=20 > "$out/railing.txt" \
            || { cat "$out/railing.txt"; echo "FAIL the railing's post steps inside its blur"; exit 1; }
        passed
    fi
    echo "PASS the railing's post blurs with no step inside its outline"
else
    echo "SKIP the railing's post: no .work/dof/frames/frame-20261029-143455"
fi
# The meadow, frame-20261029-172626: a flower close to a 77 mm lens at f/1.2 focused on Furina's eye, over a background far
# behind focus, through the display place at 4K with the user's lens. Inside the flower's blur, neighbors may differ by at
# most 6 levels: what it hides was filled from the background's sharp texels, so its translucent edge showed the
# background's detail in a ragged rim, 13.7 levels a step, up against its own blur just outside.
meadow=$frames/frame-20261029-172626
if [ -n "$frames" ] && [ -f "$meadow/frame.tsv" ]; then
    if fresh "$out/render_dof" "$meadow" render_dof.sh meadow; then
        RENDER_DOF_BIN="$out/render_dof" sh render_dof.sh "$meadow" "$out/meadow.png" f=1.2 mm=77 focus=0 x=0.73 y=0.27 max_blur=0.02 sa=0.5 chromatic=1 \
            cat_eye=0.7 scatter=1 gain=4 threshold=1.5 samples=289 roundness=0.1 display=1 upscale=3 crop=1950,1350,450,450 near_steps=2 most_steps=6 \
            > "$out/meadow.txt" || { cat "$out/meadow.txt"; echo "FAIL the meadow's flower shows a ragged rim inside its blur"; exit 1; }
        passed
    fi
    echo "PASS the meadow's flower blurs into the background behind it with no ragged rim"
else
    echo "SKIP the meadow's flower: no .work/dof/frames/frame-20261029-172626"
fi
# The meadow at dusk, frame-20261029-182125: grass, flowers and bushes at many depths in front of a 101 mm lens at f/1.2
# focused on the trees 57 m away. Inside the blurred foreground left of the middle, neighbors may differ by at most 10
# levels: a farther surface in front of focus behind a nearer one was left to the far estimate pixel by pixel, in ragged
# streaks through the bushes and a jagged rim where a flower overlapped the grass, 23.6 levels a step; and with the depth
# bands, what a thin blurred blade hid was filled in from the rest of the blade or from the grass the blade hid in turn,
# 20 levels a step.
dusk=$frames/frame-20261029-182125
if [ -n "$frames" ] && [ -f "$dusk/frame.tsv" ]; then
    if fresh "$out/render_dof" "$dusk" render_dof.sh dusk; then
        RENDER_DOF_BIN="$out/render_dof" sh render_dof.sh "$dusk" "$out/dusk.png" f=1.2 mm=101 focus=57.12 max_blur=0.02 sa=0.5 chromatic=1 cat_eye=0.7 \
            scatter=1 gain=4 threshold=1.5 samples=289 roundness=0.1 display=1 upscale=3 crop=1200,1300,600,350 near_steps=30 most_steps=10 \
            > "$out/dusk.txt" || { cat "$out/dusk.txt"; echo "FAIL the dusk meadow's blurred foreground shows ragged layers"; exit 1; }
        passed
    fi
    echo "PASS the dusk meadow's blurred foreground blurs its layers into each other"
else
    echo "SKIP the dusk meadow: no .work/dof/frames/frame-20261029-182125"
fi
# The arm, frame-20261029-210604: thin grass blades 2 to 4.5 m in front of a 152 mm lens at f/1.2 focused on the character's
# arm 5.6 m away, over a background far behind. Inside the blades over the arm, neighbors may differ by at most 26 levels:
# what a blade hid was filled in from sources out to their own discs, so the background's wide discs outweighed the arm
# right beside the blade and showed through it in shards, 32 levels a step.
arm=$frames/frame-20261029-210604
if [ -n "$frames" ] && [ -f "$arm/frame.tsv" ]; then
    if fresh "$out/render_dof" "$arm" render_dof.sh arm; then
        RENDER_DOF_BIN="$out/render_dof" sh render_dof.sh "$arm" "$out/arm.png" f=1.2 mm=152 focus=5.6 max_blur=0.02 sa=0.5 chromatic=1 cat_eye=0.7 \
            scatter=1 gain=4 threshold=1.5 samples=289 roundness=0.1 display=1 upscale=3 crop=1950,1250,300,400 near_steps=5 most_steps=26 \
            > "$out/arm.txt" || { cat "$out/arm.txt"; echo "FAIL the background shows through the blurred grass over the arm in shards"; exit 1; }
        passed
    fi
    echo "PASS the blurred grass over the arm shows the arm behind it, not the background in shards"
else
    echo "SKIP the arm: no .work/dof/frames/frame-20261029-210604"
fi
# The maples, frame-20261124-140254: maples 20 to 40 m away against the sky, behind a fruit stand 15 m away in focus, at
# f/1.2 as a 1:25 miniature with Max blur 8, live. Over the maples' blur, neighbors may differ by at most 30 levels: the
# sky through gaps in the leaves found only nearer texels around it and kept its sharp color, in white specks, and a pixel
# behind focus took or refused a blurred maple's texel by the CoC of its own sharp depth, which traced the leaves' sharp
# outline through the blur, 177 levels a step.
maples=$frames/frame-20261124-140254
if [ -n "$frames" ] && [ -f "$maples/frame.tsv" ]; then
    if fresh "$out/render_dof" "$maples" render_dof.sh maples; then
        RENDER_DOF_BIN="$out/render_dof" sh render_dof.sh "$maples" "$out/maples.png" f=1.2 sensor=573.6 x=0.458 y=0.582 max_blur=0.08 sa=0.5 \
            chromatic=1 cat_eye=0.7 scatter=1 gain=0 threshold=1.5 samples=49 crop=1115,0,125,110 near_steps=1e30 most_steps=30 \
            > "$out/maples.txt" || { cat "$out/maples.txt"; echo "FAIL the blurred maples show sharp specks or a ragged rim against the sky"; exit 1; }
        passed
    fi
    echo "PASS the blurred maples blur into the sky with no specks or ragged rim"
else
    echo "SKIP the maples: no .work/dof/frames/frame-20261124-140254"
fi
# The lens reference, .work/dof/lens-reference, which lens_reference.py writes with 144 samples: sky, hills, a banner,
# a maple with gaps, nearer leaves over it, a subject in focus and a blurred foreground, as a thin lens at f/1.2 on a 1:25
# miniature images them, hidden surfaces included. The live gather's mean error by region, in 8-bit levels, may not grow
# past what it reached: a farther surface behind focus read from the deepest light turned the nearer leaves into pale
# ghosts of themselves, 13.6 levels over them and 7.6 over the banner's rim.
lens_ref=${frames%/frames}/lens-reference
if [ -n "$frames" ] && [ -f "$lens_ref/ref.npy" ] && python3 -c 'import numpy, PIL' 2> /dev/null; then
    if fresh "$out/render_dof" "$lens_ref" render_dof.sh lens_compare.py lens; then
        RENDER_DOF_BIN="$out/render_dof" sh render_dof.sh "$lens_ref/frame" "$out/lens.png" f=1.2 sensor=573.6 focus=15 max_blur=0.08 sa=0 \
            cat_eye=0 chromatic=0 scatter=1 gain=0 contrast=0 samples=49 > /dev/null
        python3 lens_compare.py "$lens_ref" "live=$out/lens.png" "most:whole frame=2.2" "most:in focus, inside=0.4" \
            "most:far, layer edges=5.5" "most:sky in leaf gaps=8.8" "most:foreground blur=1.7" "most:near leaves on leaves=12.2" \
            "most:banner and rim=5.9" > "$out/lens.txt" || { cat "$out/lens.txt"; echo "FAIL the depth of field strays from the thin lens's picture"; exit 1; }
        passed
    fi
    echo "PASS the depth of field stays within its errors of the thin lens's picture, region by region"
else
    echo "SKIP the lens reference: no .work/dof/lens-reference, or no numpy or PIL"
fi
# The harbor, frame-20261024-134128, written as a DNG with frame_dng's test EXIF: byte for byte the reference file
# (.work/dng/lightroom-test/harbor-headroom.dng, the layout the user opened in Lightroom on 2026-10-29 with the look table
# of 2026-11-16 and the curve over the headroom of 2026-11-27), which sips must decode at the frame's size.
harbor=$frames/frame-20261024-134128
prototype=${frames%/dof/frames}/dng/lightroom-test/harbor-headroom.dng
if [ -n "$frames" ] && [ -f "$harbor/frame.tsv" ] && [ -f "$prototype" ]; then
    "$out/frame_dng" "$harbor" "$out/harbor.dng" exif > /dev/null
    cmp -s "$out/harbor.dng" "$prototype" || { echo "FAIL frame_dng's DNG of the harbor differs from the reference file"; exit 1; }
    echo "PASS frame_dng writes the harbor's DNG byte for byte as the reference file"
    if command -v sips > /dev/null; then
        sips -s format png "$out/harbor.dng" --out "$out/harbor.png" > /dev/null 2>&1 || true
        python3 -c "$png_size" "$out/harbor.png" 1152 720 2> /dev/null || { echo "FAIL sips does not decode the harbor's DNG into a 1152x720 PNG"; exit 1; }
        echo "PASS sips decodes the harbor's DNG into a 1152x720 PNG"
    fi
    rm -f "$out/harbor.dng" "$out/harbor.png"
else
    echo "SKIP the harbor's DNG: no .work/dof/frames/frame-20261024-134128 or .work/dng/lightroom-test/harbor-headroom.dng"
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
BAKE_LIGHT_BIN="$out/bake_light" sh check_light.sh "$out/scene-light-synthetic" "$out/trace-light-synthetic"
cp -R "$out/scene-light-synthetic" "$out/scene-light-broken"
python3 -c "import sys; p = sys.argv[1]; b = bytearray(open(p, 'rb').read()); b[0::4] = bytes(min(v + 40, 255) for v in b[0::4]); open(p, 'wb').write(b)" \
    "$out/scene-light-broken/face-6-albedo.rgba8"
if BAKE_LIGHT_BIN="$out/bake_light" sh check_light.sh "$out/scene-light-broken" > "$out/broken-light.txt" 2>&1; then
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
    if fresh "$out/check_bvh" "$data" check_bvh.sh check_bvh.cpp; then
        CHECK_BVH_BIN="$out/check_bvh" sh check_bvh.sh "$data"
        passed
    else
        echo "PASS check_bvh.sh on $(basename "$data"), as at its last pass"
    fi
else
    echo "SKIP check_bvh.sh: no scene-194351-tris.bin (set ENHANCEMENTS_TRACE_DATA to run it)"
fi
[ "$unchanged" = 0 ] || echo "INFO $unchanged saved-frame and scene checks were unchanged since their last pass and did not run again (FULL=1 runs them)"
