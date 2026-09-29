#!/usr/bin/env python3
"""Write CameraTools' photo mode layout from two GenshinSceneSnapshot dumps of the open photo mode settings panel.

Rerun it from anywhere after taking new snapshots:

    python3 mods/CameraTools/tools/extract_photo_layout.py

It reads .work/dumps/photomode_controller.jsonl (controller hints showing) and .work/dumps/photomode_kbm.jsonl (keyboard
hints showing) from the melonloader checkout around this repo, and writes CameraTools/PhotoModeLayout.json, which the
build embeds. The output depends only on the snapshots. It keeps names, rects, and plain uGUI styling, never game art:
sprites and fonts are stored by name and looked up in the running game.

--check SNAPSHOT.jsonl names the layout's sprites and fonts that snapshot's loaded-asset list lacks.
"""

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
DUMPS = HERE.parents[2] / ".work" / "dumps"
OUTPUT = HERE.parent / "CameraTools" / "PhotoModeLayout.json"

PAGE = "/Canvas/Pages/InLevelPhotographContext"
# The parts of GrpCom that CameraTools shows. Their ancestors are kept with only these children.
BRANCHES = [
    "GrpCom/GrpLeft/GrpSetUp",
    "GrpCom/GrpTab/Tab",
    "GrpCom/GrpTab/BtnBack",
    "GrpCom/GrpAction_PS4",
    "GrpCom/GrpActionTop_PS4",
    "GrpCom/GrpAction_PC",
    "GrpCom/Reminder_1",
    "GrpCom/GrpMain/Zoom_Slider",
]
# The glyphs are set by CameraUi, and the keyboard legend's mouse icon stays hidden.
OPTIONAL_SPRITES = ("UI_KeyXbox_", "UI_KeyPC_Mouse3")


def load(path):
    header, nodes, assets = None, {}, None
    with open(path, encoding="utf-8") as lines:
        for line in lines:
            item = json.loads(line)
            if item.get("header"):
                header = item
            elif "loadedSprites" in item:
                assets = item
            elif "path" in item and (item["path"] == PAGE or item["path"].startswith(PAGE + "/")) \
                    or item.get("path") in ("/Canvas", "/Canvas/Pages"):
                nodes[item["path"]] = item
    return header, nodes, assets


def components(item):
    return {component["type"]: component["details"] for component in item["components"]}


def rect(details):
    return {key: details[key] for key in ("anchorMin", "anchorMax", "pivot", "anchoredPosition", "sizeDelta", "scale", "rotation")}


def convert(item, dropped):
    node = {"name": item["path"].rsplit("/", 1)[-1], "active": item["activeSelf"]}
    for component in item["components"]:
        kind, details = component["type"], component["details"]
        if kind == "UnityEngine.RectTransform":
            node["rect"] = rect(details)
        elif kind == "UnityEngine.UI.Image":
            node["image"] = {key: details[key] for key in
                             ("enabled", "sprite", "imageType", "fillCenter", "fillMethod", "fillAmount", "preserveAspect", "color")}
        elif kind == "UnityEngine.UI.Text":
            node["text"] = {key: details[key] for key in
                            ("enabled", "text", "font", "fontSize", "fontStyle", "alignment", "lineSpacing", "richText",
                             "horizontalOverflow", "verticalOverflow", "bestFit", "color")}
        elif kind in ("UnityEngine.UI.Outline", "UnityEngine.UI.Shadow"):
            node.setdefault("effects", []).append({"outline": kind == "UnityEngine.UI.Outline", **details})
        elif kind in ("UnityEngine.UI.HorizontalLayoutGroup", "UnityEngine.UI.VerticalLayoutGroup"):
            node["layoutGroup"] = {"vertical": kind == "UnityEngine.UI.VerticalLayoutGroup", **details}
        elif kind == "UnityEngine.UI.LayoutElement":
            node["layoutElement"] = details
        elif kind == "UnityEngine.UI.ContentSizeFitter":
            node["fitter"] = details
        elif kind == "UnityEngine.CanvasGroup":
            node["groupAlpha"] = details["alpha"]
        elif kind == "UnityEngine.Canvas":
            node["canvas"] = {"overrideSorting": details["overrideSorting"], "sortingOrder": details["sortingOrder"]}
        elif kind != "UnityEngine.CanvasRenderer":
            dropped[kind] += 1
    if "rect" not in node:
        raise SystemExit(f"{item['path']} has no RectTransform")
    return node


def children_of(nodes, path):
    prefix = path + "/"
    return [child for child in nodes if child.startswith(prefix) and "/" not in child[len(prefix):]]


def build(path, relative, snapshots, dropped, counts, branch):
    """The node from the first snapshot that has it; the children from the first snapshot that has any."""
    item = next(nodes[path] for nodes in snapshots if path in nodes)
    node = convert(item, dropped)
    if branch is None and relative in BRANCHES:
        branch = relative
    if branch is not None:
        counts[branch] += 1
        names = next((children_of(nodes, path) for nodes in snapshots if children_of(nodes, path)), [])
    else:
        names = [child for child in children_of(snapshots[0], path)
                 if any(target == child[len(PAGE) + 1:] or target.startswith(child[len(PAGE) + 1:] + "/") for target in BRANCHES)]
    node["children"] = [build(child, child[len(PAGE) + 1:], snapshots, dropped, counts, branch) for child in names]
    return node


def walk(node):
    yield node
    for child in node["children"]:
        yield from walk(child)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--controller", type=Path, default=DUMPS / "photomode_controller.jsonl")
    parser.add_argument("--keyboard", type=Path, default=DUMPS / "photomode_kbm.jsonl")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--check", type=Path, action="append", default=[])
    args = parser.parse_args()

    header, controller, _ = load(args.controller)
    _, keyboard, _ = load(args.keyboard)
    dropped, counts = Counter(), Counter()
    root = build(PAGE, "", [controller, keyboard], dropped, counts, None)
    missing = [branch for branch in BRANCHES if counts[branch] == 0]
    if missing:
        raise SystemExit(f"the snapshots lack {', '.join(missing)}")

    nodes = list(walk(root))
    sprites = sorted({node["image"]["sprite"] for node in nodes if node.get("image", {}).get("sprite")})
    required = [sprite for sprite in sprites if not sprite.startswith(OPTIONAL_SPRITES)]
    fonts = sorted({node["text"]["font"] for node in nodes if node.get("text", {}).get("font")})
    canvas = components(controller["/Canvas"])["UnityEngine.UI.CanvasScaler"]
    layout = {
        "screen": header["screen"],
        "canvasScaler": {key: canvas[key] for key in
                         ("referenceResolution", "screenMatchMode", "matchWidthOrHeight", "referencePixelsPerUnit")},
        "pages": rect(components(controller["/Canvas/Pages"])["UnityEngine.RectTransform"]),
        "requiredSprites": required,
        "optionalSprites": [sprite for sprite in sprites if sprite not in required],
        "fonts": fonts,
        "root": root,
    }
    # Lists of numbers and flags stay on one line, so the file reads one field per line.
    text = re.sub(r"\[[^\[\]{}\"]*\]", lambda match: " ".join(match.group().split()),
                  json.dumps(layout, indent=1, sort_keys=True, ensure_ascii=False)) + "\n"
    args.output.write_text(text, encoding="utf-8")

    print(f"wrote {args.output} ({len(nodes)} nodes, {len(text)} bytes)")
    for branch in BRANCHES:
        print(f"  {counts[branch]:4} nodes  {branch}")
    print(f"{len(sprites)} distinct sprites, {len(required)} required: {', '.join(sprites)}")
    print(f"{len(fonts)} distinct fonts: {', '.join(fonts)}")
    print("dropped components: " + ", ".join(f"{kind} x{count}" for kind, count in sorted(dropped.items())))
    for path in args.check:
        _, _, assets = load(path)
        loaded_sprites = {sprite["name"] for sprite in assets["loadedSprites"]}
        loaded_fonts = {font["name"] for font in assets["loadedFonts"]}
        lacking = [sprite for sprite in required if sprite not in loaded_sprites]
        lacking_fonts = [font for font in fonts if font not in loaded_fonts]
        print(f"{path.name} lacks {len(lacking)} required sprites and {len(lacking_fonts)} fonts: "
              f"{', '.join(lacking + lacking_fonts) or 'none'}")


if __name__ == "__main__":
    sys.exit(main())
