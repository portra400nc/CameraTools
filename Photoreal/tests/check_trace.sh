#!/bin/sh
# Checks a frame PhotorealSaveTrace saved on the Deck: builds trace_reference.cpp optimized, traces the frame again on
# the Mac with the compute shader's own rays and traversal (the scene folder holding trace-scene.bin defaults to the one
# trace.tsv names, beside the trace folder), and compares with check_trace.py, which writes check-trace.png into it.
#
#   tests/check_trace.sh <trace folder> [scene folder]
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
c++ -std=c++17 -O2 -pthread -Wall -Wextra -Werror -I "$here/.." "$here/trace_reference.cpp" "$here"/../core/*.cpp -o "$out/trace_reference"
"$out/trace_reference" "$@"
python3 "$here/check_trace.py" "$1"
