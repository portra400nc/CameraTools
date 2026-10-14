#!/bin/sh
# Prints the grade layout core/grade.cpp finds among the pixel shaders of FrameCensus captures: one shader of each
# capture, its tone map, should have one. Not part of run.sh, since the captures live in .work.
#
#   tests/check_grade.sh [capture folder]...
#
# Without folders it takes capture-20261010-111824 and capture-20261009-214859 from .work/logs/FrameCensus in a folder
# above this one.
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
if [ $# -eq 0 ]; then
    dir=$here
    while [ "$dir" != "/" ] && [ ! -d "$dir/.work/logs/FrameCensus" ]; do dir=$(dirname "$dir"); done
    set -- "$dir/.work/logs/FrameCensus/capture-20261010-111824" "$dir/.work/logs/FrameCensus/capture-20261009-214859"
fi
c++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$here/.." "$here/grade_census.cpp" "$here"/../core/*.cpp -o "$out/grade_census" || exit 1
for capture in "$@"; do
    "$out/grade_census" "$capture"/shaders/*.ps.dxbc
done
