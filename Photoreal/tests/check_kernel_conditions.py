"""Checks that the trace kernel's shared HLSL calls no costly function inside &&, || or ?:.

HLSL evaluates both sides of &&, || and ?: (vkd3d, like fxc, compiles them without short-circuiting), while C++
short-circuits them, so a call there runs on the GPU when the Mac's twin of the kernel skips it. Build 204's
`h.x < hit.t && ... && !alpha_keeps(...)` (now alpha_test) ran an alpha program for every triangle the Deck's rays tested and hung its
GPU, which the Mac could not see. A function is costly when its body holds a loop or calls a costly function.

    python3 check_kernel_conditions.py [shaders folder]

Prints PASS or FAIL lines like the tests, and exits non-zero on a FAIL.
"""
import pathlib
import re
import sys

SHARED = ("trace.hlsli", "bvh.hlsli", "alpha.hlsli", "trace_cs.hlsl")
DEFINITION = re.compile(r"^[A-Za-z_]\w*\s+([A-Za-z_]\w*)\s*\(", re.M)
CONTROL = re.compile(r"^\s*(?:else\s+)?(?:if|while|for)\s*\(")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def bodies(text):
    """Each function's name and body, from its definition at the start of a line to its matching brace."""
    out = {}
    for m in DEFINITION.finditer(text):
        start = text.find("{", m.end())
        semicolon = text.find(";", m.end())
        if start < 0 or (0 <= semicolon < start):
            continue
        depth = 0
        for i in range(start, len(text)):
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            if depth == 0:
                out[m.group(1)] = text[start + 1:i]
                break
    return out


def costly_functions(functions):
    costly = {name for name, body in functions.items() if re.search(r"\bLOOP\b|\bfor\s*\(|\bwhile\s*\(", body)}
    while True:
        more = {name for name, body in functions.items() if name not in costly
                and any(re.search(r"\b%s\s*\(" % re.escape(c), body) for c in costly)}
        if not more:
            return costly
        costly |= more


def pieces(statement):
    """A statement's parts that evaluate as one expression: a control statement's condition, then the rest."""
    m = CONTROL.match(statement)
    if not m:
        return [statement]
    depth, i = 0, m.end() - 1
    for i in range(m.end() - 1, len(statement)):
        depth += {"(": 1, ")": -1}.get(statement[i], 0)
        if depth == 0:
            break
    return [statement[m.end():i]] + pieces(statement[i + 1:].strip())


def violations(text, costly):
    found = []
    for statement in re.split(r"[;{}]", strip_comments(text)):
        for piece in pieces(statement.strip()):
            if not re.search(r"&&|\|\||\?", piece):
                continue
            called = [c for c in costly if re.search(r"\b%s\s*\(" % re.escape(c), piece)]
            if called:
                found.append((" ".join(piece.split()), called))
    return found


def main():
    folder = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).parent.parent / "shaders")
    texts = {name: strip_comments((folder / name).read_text()) for name in SHARED}
    functions = {}
    for text in texts.values():
        functions.update(bodies(text))
    costly = costly_functions(functions)
    failures = 0

    def check(name, ok):
        nonlocal failures
        print(("PASS " if ok else "FAIL ") + name)
        failures += 0 if ok else 1

    check("the kernel's costly functions are found: alpha_test, visit_leaf, trace_bvh and trace_pixel_of among %d" % len(costly),
          {"alpha_test", "visit_leaf", "trace_bvh", "trace_pixel_of"} <= costly and "box_entry" not in costly)
    # Build 204's line, with the interpreter under its present name.
    old = "if (h.x < hit.t && (mask & kAlphaTests) != 0u && (tri.info & kOpaqueRecord) != kOpaqueRecord && " \
          "alpha_test(tri.info & kOpaqueRecord, h.y, h.z, lane).keeps == 0u)\n    continue;"
    check("build 204's alpha test in a chain of && is refused", len(violations(old, costly)) == 1)
    check("a costly call in ?: is refused", len(violations("hit = any ? trace_bvh(o, d, t, true, m, l) : hit;", costly)) == 1)
    check("a costly call in an if's body after a condition with && is allowed",
          not violations("if (a && b)\n    hit = visit_leaf(hit, link, o, d, any, mask, lane);", costly))
    for name, text in texts.items():
        found = violations(text, costly)
        for piece, called in found:
            print("  %s: %s calls %s" % (name, piece, ", ".join(called)))
        check("%s calls no costly function inside &&, || or ?:" % name, not found)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
