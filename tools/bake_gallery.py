#!/usr/bin/env python3
"""Pack photos for the gallery — the macOS twin of bake_gallery.ps1, plus patches.

The PowerShell baker needs Windows (System.Drawing), so this exists to do the same
job on a Mac. Same crop, same captions, same pack; see that file for the parts this
one copies rather than invents. The crop in particular is copied exactly — a portrait
keeps more of its top than its bottom (y = (h-s)/6), and mouths.csv is measured
against that crop, so changing it invalidates every mouth.

Baseline JPEG only (-interlace none): TJpgDec on the device cannot read progressive,
which is why a photo is never packed as it came.

WHAT IS NEW HERE: patches (the "GLR3" pack).

A photograph cannot talk by sliding its jaw over a dark hole — that is what the
GLR2 pack could do, and it looks like a puppet because there is no mouth interior in
a still photo to reveal. So a photo may instead carry *variants*: the same frame with
only the mouth, or only the eyes, redrawn. gallery/variants/<stem>/ holds them:

    _base.png     the neutral frame the device actually displays
    mbp.png small.png mid.png wide.png round.png pucker.png teeth.png
    blink.png smile.png surprised.png

Each is diffed against _base.png and only the rectangle that CHANGED is stored, as
RGB565A8 — LVGL's own layout, so the device points an lv_image_dsc_t straight at
flash and blits it. No decode, no PSRAM, no per-frame cost: swapping a viseme is one
lv_image_set_src(). A mouth shape costs ~10 KB where a whole re-encoded frame costs
60 KB and a 180 ms decode, which is far more than a viseme's lifetime.

The stored box is deliberately TIGHTER than the measured difference. An image model
blends its edit into the original over a soft boundary, which leaves a faint halo;
cropping inside that ring and feathering our own alpha discards it. The skin just
outside the lips is identical in both frames, so cutting tight costs nothing.
"""
import argparse, io, os, re, struct, subprocess, sys, unicodedata

NAME_LEN = 24        # gallery.c NAME_LEN, terminator included
MAX_PHOTOS = 64      # gallery.c MAX_PHOTOS
MAX_PATCHES = 16     # gallery.c MAX_PATCHES
EXTS = (".jpg", ".jpeg", ".png", ".bmp", ".gif")

#: role -> id. 0..7 are the viseme classes in VISEMES order (lipsync.h), so the
#: device indexes straight from lipsync_now(). REST is not stored: it IS _base.
ROLES = {"mbp": 1, "small": 2, "mid": 3, "wide": 4, "round": 5, "pucker": 6,
         "teeth": 7, "blink": 8, "smile": 9, "surprised": 10}
PATCH_INSET = 3      # px stepped inside the model's blend ring
PATCH_FEATHER = 2.5  # px of alpha falloff at the patch edge


def caption(filename):
    s = os.path.splitext(filename)[0]
    s = re.sub(r"^\d+[\s._-]*", "", s)
    s = re.sub(r"[_-]+", " ", s)
    s = s.replace("đ", "d").replace("Đ", "D")
    s = "".join(c for c in unicodedata.normalize("NFD", s)
                if unicodedata.category(c) != "Mn")
    s = "".join(c for c in s if 32 <= ord(c) < 127)
    return s.strip().title()[:NAME_LEN - 1]


def magick(*args, binary=True):
    r = subprocess.run(["magick", *args], capture_output=True, check=True)
    return r.stdout if binary else r.stdout.decode()


def convert(path, side, quality):
    wh = magick("identify", "-format", "%w %h", path + "[0]", binary=False).split()
    w, h = int(wh[0]), int(wh[1])
    s = min(w, h)
    x, y = (w - s) // 2, (h - s) // 6
    out = magick(path + "[0]", "-auto-orient",
                 "-crop", "%dx%d+%d+%d" % (s, s, x, y), "+repage",
                 "-resize", "%dx%d!" % (side, side),
                 "-colorspace", "sRGB", "-strip",
                 "-interlace", "none", "-quality", str(quality), "jpg:-")
    if not out.startswith(b"\xff\xd8"):
        raise RuntimeError("not a JPEG")
    return out


def mouths(indir, side):
    """stem -> (x, y, w, jaw) in pixels of the crop; fractions in the file."""
    table, path = {}, os.path.join(indir, "mouths.csv")
    if not os.path.exists(path):
        return table
    for line in io.open(path, encoding="utf-8"):
        f = [p.strip() for p in line.strip().split(",")]
        if len(f) != 5 or f[0] in ("stem", "") or f[0].startswith("#"):
            continue
        v = [float(p) for p in f[1:]]
        if any(p < 0 or p > 1 for p in v):
            print("mouths.csv: %s is not in 0..1; ignored" % f[0], file=sys.stderr)
            continue
        table[f[0]] = tuple(int(round(p * side)) for p in v)
    return table


def diff_box(base_png, var_png):
    """The rectangle that actually changed, from ImageMagick."""
    magick(base_png, var_png, "-compose", "difference", "-composite",
           "-colorspace", "gray", "-threshold", "12%",
           "-morphology", "close", "disk:2", "/tmp/_gb.png")
    out = magick("/tmp/_gb.png", "-format", "%@", "info:", binary=False).strip()
    wh, x, y = out.split("+")
    w, h = (int(v) for v in wh.split("x"))
    return int(x), int(y), w, h


def patch(base_png, var_png, side, clip_below=None):
    """One patch: tight crop, our own feathered alpha, RGB565A8.

    `clip_below` cuts the box off above a y: a blink must not carry the mouth with
    it. The variants were generated from the ORIGINAL frame, whose mouth is open,
    while _base is the neutral closed mouth — so a raw difference for blink.png
    reports the eyes AND the mouth, and compositing it would re-open the mouth every
    time the photo blinked. Clipping is cheaper than regenerating the art against a
    different source, and it is also simply true: a blink changes eyes, nothing else.
    """
    x, y, w, h = diff_box(base_png, var_png)
    if clip_below is not None and y + h > clip_below:
        h = max(0, clip_below - y)
    x, y = x + PATCH_INSET, y + PATCH_INSET
    w, h = w - 2 * PATCH_INSET, h - 2 * PATCH_INSET
    if w < 8 or h < 8:
        return None
    # even dimensions keep the colour plane 2-byte aligned on the device
    w, h = w & ~1, h & ~1
    px = magick(var_png, "-crop", "%dx%d+%d+%d" % (w, h, x, y), "+repage",
                "-resize", "%dx%d!" % (w, h), "-depth", "8", "RGBA:-")
    f = PATCH_FEATHER
    magick("-size", "%dx%d" % (w, h), "xc:black", "-fill", "white",
           "-draw", "roundrectangle %.1f,%.1f %.1f,%.1f %d,%d"
           % (f, f, w - 1 - f, h - 1 - f, h // 4, h // 4),
           "-blur", "0x%.1f" % f, "-depth", "8", "GRAY:/tmp/_ga.gray")
    alpha = io.open("/tmp/_ga.gray", "rb").read()[:w * h]
    # RGB565A8 as LVGL stores it: the whole colour plane, then the whole alpha
    # plane. Not interleaved — a per-pixel 3-byte layout renders as noise.
    colour = bytearray(w * h * 2)
    for i in range(w * h):
        r, g, b = px[i * 4], px[i * 4 + 1], px[i * 4 + 2]
        struct.pack_into("<H", colour, i * 2,
                         ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return x, y, w, h, bytes(colour) + alpha


def photo_patches(indir, stem, side, quality, mouth_y=0):
    """(base_jpeg, [(role, x, y, w, h, data)]) for one photo, or (None, []) if it
    has no variants directory — an ordinary GLR2 photo."""
    vdir = os.path.join(indir, "variants", stem)
    base = os.path.join(vdir, "_base.png")
    if not os.path.isdir(vdir) or not os.path.exists(base):
        return None, []
    out = []
    for name, role in sorted(ROLES.items(), key=lambda kv: kv[1]):
        f = os.path.join(vdir, name + ".png")
        if not os.path.exists(f):
            continue
        # eyes-only roles stop above the mouth; see patch()
        clip = (mouth_y - side // 22) if (role == 8 and mouth_y) else None
        p = patch(base, f, side, clip_below=clip)
        if p is None:
            print("  %-10s skipped (nothing changed)" % name, file=sys.stderr)
            continue
        x, y, w, h, data = p
        out.append((role, x, y, w, h, data))
        print("  %-10s %3dx%-3d at %3d,%-3d  %6.1f KB" % (name, w, h, x, y, len(data) / 1024))
    return convert(base, side, quality), out[:MAX_PATCHES]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="indir", default="gallery")
    ap.add_argument("--out", default="")
    ap.add_argument("--side", type=int, default=480, choices=(480, 720))
    ap.add_argument("--quality", type=int, default=85)
    a = ap.parse_args()
    out = a.out or os.path.join(a.indir, "gallery.bin" if a.side == 480
                                else "gallery-%d.bin" % a.side)

    mouth = mouths(a.indir, a.side)
    files = sorted(f for f in os.listdir(a.indir)
                   if f.lower().endswith(EXTS) and not f.startswith("."))
    if not files:
        sys.exit("No photos in '%s'." % a.indir)

    items = []
    for f in files[:MAX_PHOTOS]:
        stem = os.path.splitext(f)[0]
        try:
            print("%s:" % f)
            jpg, patches = photo_patches(a.indir, stem, a.side, a.quality,
                                         mouth_y=mouth.get(stem, (0, 0, 0, 0))[1])
            if jpg is None:                       # no variants: the plain photo
                jpg, patches = convert(os.path.join(a.indir, f), a.side, a.quality), []
                print("  (no variants; jaw puppet only)")
            items.append((caption(f), jpg, mouth.get(stem, (0, 0, 0, 0)), patches))
        except Exception as e:
            print("%s: skipped (%s)" % (f, e), file=sys.stderr)

    ENTRY, PATCH = 48, 16
    n = len(items)
    # entries, then each photo's patch table, then the JPEGs, then patch data
    off = 8 + n * ENTRY
    ptab = {}
    for i, (_, _, _, patches) in enumerate(items):
        ptab[i] = off
        off += len(patches) * PATCH
    jpg_off = off
    for _, jpg, _, _ in items:
        off += len(jpg)
    data_off = off

    blob_entries, blob_tables, blob_jpgs, blob_data = bytearray(), bytearray(), bytearray(), bytearray()
    jo, do = jpg_off, data_off
    for i, (name, jpg, m, patches) in enumerate(items):
        blob_entries += struct.pack("<II", jo, len(jpg))
        blob_entries += name.encode("ascii", "ignore")[:NAME_LEN - 1].ljust(NAME_LEN, b"\0")
        blob_entries += struct.pack("<4H", *m)
        blob_entries += struct.pack("<HHI", len(patches), 0, ptab[i])
        jo += len(jpg)
        blob_jpgs += jpg
        for role, x, y, w, h, data in patches:
            blob_tables += struct.pack("<4H", x, y, w, h)
            blob_tables += struct.pack("<BBHI", role, 0, 0, do)
            do += len(data)
            blob_data += data

    with open(out, "wb") as fh:
        fh.write(b"GLR3" + struct.pack("<I", n))
        fh.write(blob_entries); fh.write(blob_tables)
        fh.write(blob_jpgs); fh.write(blob_data)

    print("\n%s: %d photo(s), %.0f KB  (%.0f KB photos, %.0f KB patches)"
          % (out, n, os.path.getsize(out) / 1024,
             len(blob_jpgs) / 1024, len(blob_data) / 1024))


main()
