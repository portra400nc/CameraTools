#!/bin/sh
# Checks a ENHANCEMENTS_SCENE_LIGHT | GBUFFER capture's light on the Mac: builds bake_light.cpp optimized, bakes the light
# again from the faces' raw copies and checks it against trace-light.bin to the bit, and with a trace folder of a save at
# the capture's spot compares the albedo the view's rays meet with the G-buffer's; then check_light.py reports the
# sources by gather hit, writes the sky map beside the six faces, and fits the sun's color to the saved view.
#
#   tests/check_light.sh <scene folder> [trace folder]
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
c++ -std=c++17 -O2 -pthread -Wall -Wextra -Werror -I "$here/.." "$here/bake_light.cpp" "$here"/../core/*.cpp -o "$out/bake_light" || exit 1
"$out/bake_light" "$@"
baked=$?
python3 "$here/check_light.py" "$@"
checked=$?
[ "$baked" -eq 0 ] && [ "$checked" -eq 0 ]
