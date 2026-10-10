#!/usr/bin/env python3
"""Turn a FrameCensus capture (add-on version 1 or 2) into the compact fixture the Photoreal tests replay.

The fixture keeps one row per bind, clear and draw, with each texture's format and size resolved inline, so the C++
tests need one trivial parser and no census format knowledge. This script is the only reader of the census formats.

    python3 make_fixture.py <capture folder> <fixture.tsv>

Columns: seq, event (bind|clear|draw), targets, depth, inputs, clear. A texture is <id>:<FORMAT>:<w>x<h>, a list is
comma-separated, and - is empty. targets are the bound color targets for a bind and the cleared target for a clear.
inputs are a draw's pixel shader resources, with the resource's own format, in slot order.
"""

import csv
import sys
from pathlib import Path


def main():
    capture, out = Path(sys.argv[1]), Path(sys.argv[2])
    resources = {}
    with open(capture / "resources.tsv") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            # Buffers have no format or height in the census; they become UNKNOWN:<bytes>x0, which no matcher accepts.
            dim = lambda value: value if value not in ("-", "") else "0"
            fmt = row["format"] if row["format"] not in ("-", "") else "UNKNOWN"
            resources[row["id"]] = (fmt, f"{dim(row['width'])}x{dim(row['height'])}")

    def view(text):
        # "79:R10G10B10A2_UNORM" (the census writes the view's format) -> "79:R10G10B10A2_UNORM:1152x720"
        if text in ("-", ""):
            return "-"
        rid, fmt = text.split(":")[:2]
        return f"{rid}:{fmt}:{resources[rid][1]}"

    def inputs(text):
        # v2 "slot:id,...", v1 "id,..." in bound order; either way the resource's own format.
        if text in ("-", ""):
            return "-"
        ids = [part.split(":")[-1] for part in text.split(",")]
        return ",".join(f"{i}:{resources[i][0]}:{resources[i][1]}" for i in ids if i in resources)

    rows = []
    with open(capture / "events.tsv") as f:
        for e in csv.DictReader(f, delimiter="\t"):
            kind = e["event"]
            if kind == "bind_targets":
                targets = ",".join(view(t) for t in e["rtvs"].split(",")) if e["rtvs"] != "-" else "-"
                rows.append((e["seq"], "bind", targets, view(e["dsv"]), "-", "-"))
            elif kind == "clear_rtv":
                color = e["detail"].removeprefix("color=") if e["detail"].startswith("color=") else "-"
                rows.append((e["seq"], "clear", view(e["rtvs"]), "-", "-", color))
            elif kind in ("draw", "draw_indexed", "draw_indirect"):
                rows.append((e["seq"], "draw", "-", "-", inputs(e["ps_srvs"]), "-"))

    with open(out, "w") as f:
        f.write("seq\tevent\ttargets\tdepth\tinputs\tclear\n")
        for row in rows:
            f.write("\t".join(row) + "\n")
    print(f"{out}: {len(rows)} events from {capture.name}")


if __name__ == "__main__":
    main()
