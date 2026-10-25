#!/bin/sh
# Builds check_bvh.cpp optimized and runs it on a whole scene capture's deduplicated triangles, the file it names
# (.work/photoreal/tracer/scene-194351-tris.bin in the melonloader workspace): the build's time on 1 and 8 threads, the
# same result on both, the traversal against every-triangle testing, its rays a second, and the fixture's BVH hash
# bvh_test.cpp expects from an optimized build. run.sh runs it when it finds the file.
set -e
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
c++ -std=c++17 -O2 -pthread -Wall -Wextra -Werror -I .. check_bvh.cpp ../core/*.cpp -o "$out/check_bvh"
"$out/check_bvh" "$1" 5acc3b0243e8c541
