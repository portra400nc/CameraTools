#!/bin/sh
# Checks a frame EnhancementsSaveTrace saved on the Deck: builds trace_reference.cpp optimized, traces the frame again on
# the Mac with the compute shader's own rays and traversal (the scene folder holding trace-scene.bin defaults to the one
# trace.tsv names, beside the trace folder), and compares with check_trace.py, which writes check-trace.png into it.
# check_trace.py runs whatever trace_reference found, as long as it wrote reference.f32, and the script fails when either
# failed.
#
#   tests/check_trace.sh <trace folder> [scene folder]
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
c++ -std=c++17 -O2 -pthread -Wall -Wextra -Werror -I "$here/.." "$here/trace_reference.cpp" "$here"/../core/*.cpp -o "$out/trace_reference" || exit 1
rm -f "$1/reference.f32"
"$out/trace_reference" "$@"
traced=$?
[ -f "$1/reference.f32" ] || exit 1
python3 "$here/check_trace.py" "$1"
checked=$?
[ "$traced" -eq 0 ] && [ "$checked" -eq 0 ]
