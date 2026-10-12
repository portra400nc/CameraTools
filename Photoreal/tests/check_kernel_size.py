"""Checks that the compiled trace kernel stays the size the Deck traced fast with.

Build 203's kernel, without alpha tests, traced 38 to 50 million rays a second on the Deck with 41 temporaries, 1,237
instructions and 6,212 bytes of group-shared memory, as this counts them. Build 206's, which interpreted the game's
alpha programs at each hit, had 69 temporaries, 2,782 instructions and 20,548 bytes, and traced about 300 times
slower. This compiles shaders/trace_cs.hlsl with vkd3d-compiler to DXBC, disassembles it and fails when it declares
more than 48 temporaries, any indexable temporary (scratch memory on RADV), more group-shared memory than the traversal
stacks and the counters, or more than 1,600 instructions.

    python3 check_kernel_size.py <vkd3d-compiler> [shaders folder]

Prints the numbers and PASS or FAIL lines like the tests, and exits non-zero on a FAIL.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

MAX_TEMPS = 48
MAX_INSTRUCTIONS = 1600
# The traversal stacks (64 lanes x 24 uints) and the group's counters (17 uints), with a little room for more counters.
MAX_GROUP_SHARED = 64 * 24 * 4 + 32 * 4


def measure(asm):
    """dcl_temps, indexable temporaries, group-shared bytes and instructions of a d3d-asm disassembly."""
    temps = 0
    indexable = 0
    shared = 0
    instructions = 0
    for line in asm.splitlines():
        line = line.strip()
        if not line or line.startswith("//") or re.match(r"^[a-z]s_\d_\d$", line):
            continue
        m = re.match(r"dcl_temps (\d+)", line)
        if m:
            temps = int(m.group(1))
        elif line.startswith("dcl_indexableTemp"):
            indexable += 1
        elif line.startswith("dcl_tgsm_structured"):
            _, stride, count = [p.strip() for p in line.split(",")]
            shared += int(stride) * int(count)
        elif line.startswith("dcl_tgsm_raw"):
            shared += int(line.split(",")[-1])
        elif not line.startswith("dcl_"):
            instructions += 1
    return temps, indexable, shared, instructions


def disassemble(vkd3d, shaders):
    with tempfile.TemporaryDirectory() as out:
        dxbc = pathlib.Path(out) / "trace_cs.dxbc"
        subprocess.run([str(vkd3d), "-x", "hlsl", "-b", "dxbc-tpf", "--profile", "cs_5_0", "trace_cs.hlsl", "-o", str(dxbc)],
                       cwd=shaders, check=True)
        return subprocess.run([str(vkd3d), "-x", "dxbc-tpf", "-b", "d3d-asm", str(dxbc)], check=True, capture_output=True,
                              text=True).stdout, dxbc.stat().st_size


def main():
    vkd3d = pathlib.Path(sys.argv[1]).resolve()
    shaders = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else pathlib.Path(__file__).parent.parent / "shaders").resolve()
    asm, size = disassemble(vkd3d, shaders)
    temps, indexable, shared, instructions = measure(asm)
    print(f"trace_cs.dxbc: {size} bytes, dcl_temps {temps}, indexable temporaries {indexable}, group-shared {shared} bytes, "
          f"{instructions} instructions")
    failed = False
    for ok, what in ((temps <= MAX_TEMPS, f"at most {MAX_TEMPS} temporaries"),
                     (indexable == 0, "no indexable temporaries"),
                     (shared <= MAX_GROUP_SHARED, f"at most {MAX_GROUP_SHARED} bytes of group-shared memory"),
                     (instructions <= MAX_INSTRUCTIONS, f"at most {MAX_INSTRUCTIONS} instructions")):
        print(("PASS" if ok else "FAIL") + " the trace kernel has " + what)
        failed |= not ok
    # The counting itself: a disassembly of 2 temporaries, an indexable array, 1,536 + 68 bytes and 3 instructions.
    sample = ("cs_5_0\ndcl_globalFlags refactoringAllowed\ndcl_temps 2\ndcl_indexableTemp x0[4], 4\n"
              "dcl_tgsm_structured g0, 4, 384\ndcl_tgsm_raw g1, 68\nmov r0.x, l(0)\n// comment\nadd r1.x, r0.x, l(1)\nret\n")
    ok = measure(sample) == (2, 1, 1604, 3)
    print(("PASS" if ok else "FAIL") + " check_kernel_size.py counts a sample disassembly's temporaries, arrays, memory and instructions")
    failed |= not ok
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
