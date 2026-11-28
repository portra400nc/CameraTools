"""Checks a DNG's preview and raw image with macOS's sips.

    check_dng_preview.py ramp <ramp.dng>
        The 320x200 ramp dng_test writes: its preview, decoded by sips, within JPEG's error of the ramp as the game shows it.
    check_dng_preview.py same-raw <file.dng> <reference.dng>
        The file holds the reference's raw tiles, profile and EXIF byte for byte, wherever either keeps its raw image.
"""
import os
import struct
import subprocess
import sys
import tempfile

SIZES = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 7: 1, 10: 8, 11: 4}
PLACEMENT = {254, 256, 257, 258, 259, 262, 273, 277, 278, 279, 284, 322, 323, 324, 325, 330, 339, 34665, 50717}


def ifd(d, at):
    (n,) = struct.unpack_from("<H", d, at)
    tags = {}
    for i in range(n):
        tag, typ, count = struct.unpack_from("<HHI", d, at + 2 + 12 * i)
        size = SIZES[typ] * count
        where = at + 10 + 12 * i if size <= 4 else struct.unpack_from("<I", d, at + 10 + 12 * i)[0]
        tags[tag] = (typ, count, d[where : where + size])
    return tags


def longs(tags, tag):
    typ, count, data = tags[tag]
    return struct.unpack(f"<{count}{'H' if typ == 3 else 'I'}", data)


def parts(d):
    """IFD0, the raw image's IFD and the preview's, or None."""
    first = ifd(d, struct.unpack_from("<I", d, 4)[0])
    if 330 not in first:
        return first, first, None
    subs = [ifd(d, at) for at in longs(first, 330)]
    raw = next(s for s in subs if longs(s, 254)[0] == 0)
    preview = next((s for s in subs if longs(s, 254)[0] == 1 and longs(s, 256)[0] == longs(raw, 256)[0]), None)
    return first, raw, preview


def bmp_pixels(path):
    d = open(path, "rb").read()
    start, width, height, bpp = struct.unpack_from("<I", d, 10)[0], *struct.unpack_from("<iiHH", d, 18)[:2], struct.unpack_from("<H", d, 28)[0]
    stride = (width * bpp // 8 + 3) & ~3
    rows = []
    for y in range(abs(height)):
        row = start + (abs(height) - 1 - y if height > 0 else y) * stride
        rows.append([tuple(d[row + x * bpp // 8 + k] for k in (2, 1, 0)) for x in range(width)])
    return width, abs(height), rows


def game_tone(x):
    return x * (1.36 * x + 0.047) / (x * (0.93 * x + 0.56) + 0.14)


def shown(linear):
    v = min(1.0, game_tone(linear))
    e = 12.92 * v if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055
    return round(min(max(e, 0.0), 1.0) * 255)


def check_ramp(path):
    d = open(path, "rb").read()
    _, _, preview = parts(d)
    if preview is None:
        sys.exit("FAIL the ramp's DNG has no full-size preview")
    at, size = longs(preview, 273)[0], longs(preview, 279)[0]
    with tempfile.TemporaryDirectory() as tmp:
        jpeg, bmp = os.path.join(tmp, "preview.jpg"), os.path.join(tmp, "preview.bmp")
        open(jpeg, "wb").write(d[at : at + size])
        subprocess.run(["sips", "-s", "format", "bmp", jpeg, "--out", bmp], check=True, capture_output=True)
        width, height, rows = bmp_pixels(bmp)
    if (width, height) != (320, 200):
        sys.exit(f"FAIL sips decodes the ramp's preview at {width}x{height}, not 320x200")
    errors = []
    for y in range(height):
        for x in range(width):
            r, g = 2.0 * x / width, 2.0 * y / height
            for k, v in enumerate((r, g, (r + g) / 2)):
                sample = int(min(v / 2, 1) * 65535 + 0.5)
                errors.append(abs(rows[y][x][k] - shown(2 * sample / 65535)))
    mean, worst = sum(errors) / len(errors), max(errors)
    if mean > 1.5 or worst > 12:
        sys.exit(f"FAIL the ramp's preview is {mean:.2f} levels off on average and {worst} at most, over 1.5 and 12")
    print(f"PASS sips decodes the ramp's preview as the game shows the ramp, {mean:.2f} levels off on average and {worst} at most")


def check_same_raw(path, reference):
    new, old = open(path, "rb").read(), open(reference, "rb").read()
    (new0, new_raw, _), (old0, old_raw, _) = parts(new), parts(old)
    tiles = lambda d, raw: [d[o : o + c] for o, c in zip(longs(raw, 324), longs(raw, 325))]
    raw_tags = lambda raw: {t: raw[t] for t in (256, 257, 258, 259, 262, 277, 284, 322, 323, 325, 339, 50717)}
    profile = lambda first: {t: v for t, v in first.items() if t not in PLACEMENT}
    exif = lambda d, first: ifd(d, longs(first, 34665)[0])
    if tiles(new, new_raw) != tiles(old, old_raw) or raw_tags(new_raw) != raw_tags(old_raw):
        sys.exit("FAIL the raw image differs from the reference's")
    if profile(new0) != profile(old0) or exif(new, new0) != exif(old, old0):
        sys.exit("FAIL the profile or the EXIF differs from the reference's")
    print("PASS the raw image, profile and EXIF are the reference's byte for byte")


if __name__ == "__main__":
    if sys.argv[1] == "ramp":
        check_ramp(sys.argv[2])
    else:
        check_same_raw(sys.argv[2], sys.argv[3])
