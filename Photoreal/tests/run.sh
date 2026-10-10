#!/bin/sh
# Builds and runs the Photoreal core's tests, which replay recorded census frames through the frame recipe, with any
# C++17 compiler, then decodes the PNG encoder's output with Python's zlib. No game, no ReShade, no D3D11.
set -e
cd "$(dirname "$0")"
out=$(mktemp -d)
for test in *_test.cpp; do
    c++ -std=c++17 -Wall -Wextra -Werror -I .. "$test" fixture.cpp ../core/*.cpp -o "$out/${test%.cpp}"
    "$out/${test%.cpp}" "$out"
done
python3 check_png.py "$out/roundtrip.png"
