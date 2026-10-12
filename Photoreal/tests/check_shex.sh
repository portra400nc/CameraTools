#!/bin/sh
# Decodes every shader of FrameCensus captures with core/shex.cpp and compares each instruction's name with vkd3d's
# disassembly the census wrote beside it (asm/<hash>.<stage>.asm), its resource, precise and indexable qualifiers left
# out. Not part of run.sh, since the captures live in .work.
#
#   tests/check_shex.sh [capture folder]...
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
c++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$here/.." "$here/shex_census.cpp" "$here"/../core/*.cpp -o "$out/shex_census" || exit 1
for capture in "$@"; do
    "$out/shex_census" names "$capture"/shaders/*.dxbc
done > "$out/names.txt"
python3 - "$out/names.txt" <<'PY'
import re, sys
shaders = instructions = differ = 0
for line in open(sys.argv[1]):
    path, *mine = line.split()
    theirs = []
    for text in open(path.replace("/shaders/", "/asm/").replace(".dxbc", ".asm")):
        text = re.sub(r"\s*\[precise[^\]]*\]", "", text.strip())
        if not text or text.startswith(("dcl_", "{", "}")) or re.match(r"^(ps|vs|cs|gs|hs|ds)_\d_\d$", text):
            continue
        name = text.split()[0].split("(")[0].replace("_indexable", "")
        theirs.append(re.sub(r"^sync.*", "sync", re.sub(r"^resinfo_uint", "resinfo", name)))
    shaders += 1
    instructions += len(theirs)
    if mine != theirs:
        differ += 1
        first = next((i for i, (a, b) in enumerate(zip(mine, theirs)) if a != b), min(len(mine), len(theirs)))
        print(f"FAIL {path}: instruction {first} is {mine[first:first + 1]} here and {theirs[first:first + 1]} in vkd3d's disassembly")
print(f"{'PASS' if differ == 0 else 'FAIL'} {shaders - differ} of {shaders} shaders decode to vkd3d's {instructions} instruction names")
sys.exit(1 if differ else 0)
PY
