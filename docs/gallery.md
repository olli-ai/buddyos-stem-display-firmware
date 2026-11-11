# The photo gallery

The other build this firmware carries: photographs instead of a face, and how to make one of them blink and talk.

[All docs](README.md) · [Repository README](../README.md)

---

### The gallery

The screen is your photos, one page each, with the name and a row of page dots. Swipe left
and right. This build ships with two Como 1907 players:

```
gallery/01-cesc_fabregas.jpg         -> "Cesc Fabregas"
gallery/02-nico_paz.png              -> "Nico Paz"
```

**The photos are not in the repo** — `gallery/` is gitignored, because it is other people's
pictures. **Nor are the generated mouth and eye variants** (see below): a clean clone has
no `gallery/` at all, the build fails on purpose until you make one, and everything in it
has to be regenerated. Put photos in `gallery/` (any size; JPEG, PNG, BMP or GIF; the
leading number only sets the order), then bake them:

    powershell -ExecutionPolicy Bypass -File tools\bake_gallery.ps1   # Windows
    python3 tools/bake_gallery.py --in gallery --side 720             # macOS/Linux

The Python baker is the one to use anywhere but Windows — `bake_gallery.ps1` needs
System.Drawing — and it is also the only one that packs variants. It needs ImageMagick
(`magick`) on PATH.

It turns each one upright, crops it square (keeping more of a portrait's top, where the face
is), sizes it to the panel (240×240 on the S3) and re-encodes it as a baseline JPEG, which is the only kind the
device's decoder reads. `-Demo` packs five named placeholder cards instead, to try the swipe
before choosing any photos. **The build fails without `gallery/gallery.bin`**, on purpose,
with a message saying so; a pack that is present but damaged degrades to fewer photos, or
to a line of text on the glass, never to a fault.

The pack is embedded in the app: 35–90 KB a photo, so the two come to 79 KB. A decoded
photo is 460 KB of PSRAM, so only the page showing and its two neighbours are kept.
Measured with the radio up: **~180 ms** to decode one, with PSRAM at 80 MHz, and that
happens after a swipe settles, never during one.

### Making a photo talk, and blink

A photograph has one expression and no eyelid interior, so by default all it can do is
puppet its jaw — slide the chin down over an invented dark hole. To do better, give a
photo **variants of itself**: the same frame with only the mouth, or only the eyes,
redrawn. The baker keeps just the rectangle that changed, and the device swaps it.

Two optional things live beside the photos. Neither is in the repo; both are yours to make.

**1. `gallery/mouths.csv`** — where each mouth is, for the jaw puppet. One line per photo,
as fractions of the square crop the baker makes (**not** of your original):

```
stem,x,y,w,jaw
01-cesc_fabregas,0.555,0.605,0.130,0.125
02-nico_paz,0.583,0.458,0.090,0.105
```

`x,y` is the middle of the lips' seam, `w` the mouth's width, `jaw` the seam down to the
bottom of the chin. Measure them off a baked crop, not the original photo — the crop is
what the device draws. A quick way: bake once, pull the JPEG back out of the pack, and
overlay a 10% grid with ImageMagick.

**2. `gallery/variants/<stem>/`** — the redrawn frames. `<stem>` is the photo's filename
without its extension, so `02-nico_paz.png` takes `gallery/variants/02-nico_paz/`:

```
_base.png     the neutral frame the device DISPLAYS (closed, relaxed mouth)
mbp.png  small.png  mid.png  wide.png  round.png  pucker.png  teeth.png
blink.png  smile.png  surprised.png
```

The seven mouth files are the viseme classes in `VISEMES` order (see `lipsync.h`), minus
REST — REST has no file because it *is* `_base.png`. All of them must be the same size as
each other and pixel-aligned with `_base.png`: the baker stores only the difference, so
anything that shifts the head, the crop or the lighting shows up as a visible patch.

These were produced with an image model, from one source photo each, prompted one file at
a time: *"the same photo, with ONLY the mouth changed to …; everything else pixel-identical
— same framing, crop, head position, hair, clothing, background, lighting and skin tone;
blend seamlessly with no visible edge or halo; output exactly 720×720."* Five faults were
worth naming explicitly, because they all appeared on the first attempt and all went away
when the prompt called them out: a pucker drawn as a cartoon kiss-face, a smudged `mbp`, a
`teeth` that was just a smile instead of the lower lip tucked under the upper teeth, a
`mid` barely different from the neutral frame, and a `surprised` that changed the subject's
eye colour and bulged the eyes alarmingly.

Check the result before baking: every variant's difference from `_base.png` should be a
small box over the feature it names. If a mouth's box reaches the eyes, the model redrew
the face and that frame will ghost — regenerate it rather than ship it.

#### Doing it, step by step

What follows is exactly how the two shipped players were made, on a Mac, with the
[Codex CLI](https://github.com/openai/codex) for the image generation. Any image tool that
can edit one region of a photo and leave the rest alone will do; the prompt matters more
than the tool.

**1. Crop the source square first.** Generate *from the baked crop*, not the original
photo, so the variants are aligned with what the device actually draws:

    python3 tools/bake_gallery.py --in gallery --side 720     # bakes the plain photos
    python3 - <<'EOF'
    import io, struct
    b = io.open('gallery/gallery-720.bin','rb').read()
    n, = struct.unpack('<I', b[4:8]); off = 8
    for i in range(n):
        o, sz = struct.unpack('<II', b[off:off+8]); off += 40
        io.open('crop%d.jpg' % i, 'wb').write(b[o:o+sz])
    EOF

**2. Set up one directory per photo** with the crop as `base.jpg`, and generate into it:

    mkdir -p work/02-nico_paz && cp crop1.jpg work/02-nico_paz/base.jpg
    cd work/02-nico_paz
    codex exec --skip-git-repo-check -C "$PWD" -s workspace-write 'You have base.jpg, a
    720x720 photo of a person facing the camera. Using your image generation tool, produce
    these 720x720 PNG files in this directory. Each is the SAME photo with ONLY the named
    feature changed.

    MOUTH SHAPES (eyes stay open and identical in all of these):
      mouth_closed.png  lips relaxed and closed, neutral
      v_mbp.png         lips closed and pressed together for "m"/"b"/"p" - crisp, not smudged
      v_small.png       lips barely parted, a small narrow gap
      v_mid.png         clearly half-open, vowel "eh", a visible gap and a little darkness
                        inside - must be clearly MORE open than mouth_closed.png
      v_wide.png        open wide, jaw dropped, as in "ah", teeth and dark interior visible
      v_round.png       lips rounded into an O, as in "oh"
      v_pucker.png      SUBTLE: lips lightly pushed forward and narrowed for the "oo" in
                        "food". A small natural speech shape, NOT an exaggerated kiss-face
      v_teeth.png       the LOWER LIP tucked up under the UPPER FRONT TEETH, as when saying
                        "f" or "v". The upper teeth rest visibly on the lower lip. NOT a smile

    EYES (mouth stays exactly as in base.jpg):
      eyes_closed.png   both eyes gently closed, relaxed lids, natural lashes

    EXPRESSIONS:
      smile.png         a warm closed-lip smile
      surprised.png     eyebrows raised SLIGHTLY, eyes a little wider but NOT bulging - no
                        white above the iris - mouth slightly open. Gentle curious surprise,
                        not fright. Keep the iris colour exactly as base.jpg

    CRITICAL for every file: everything outside the changed feature must be pixel-identical
    to base.jpg - same framing, crop, head position and angle, hair, clothing, background,
    lighting and skin tone. Blend seamlessly: NO visible circular edge or halo where your
    edit meets the original. Do not zoom, rotate, crop or restyle. Output exactly 720x720.'

**3. Check every file before you trust it.** The difference from `base.jpg` must be a small
box over the named feature. This prints one line per variant:

    for f in mouth_closed v_mbp v_small v_mid v_wide v_round v_pucker v_teeth \
             eyes_closed smile surprised; do
      magick base.jpg $f.png -compose difference -composite -colorspace gray \
             -threshold 12% -morphology close disk:2 /tmp/d.png
      printf "%-14s %s\n" "$f" "$(magick /tmp/d.png -format '%@' info:)"
    done

A mouth variant should report something like `75x47+388+309` — tens of pixels, over the
mouth. Hundreds of pixels, or a box reaching the eyes, means the model redrew the face:
regenerate that one with the fault named in the prompt. Then look at them, side by side,
large — the box test catches ghosting, not ugliness.

**4. Put them where the baker expects**, renaming to the role names:

    V=../../gallery/variants/02-nico_paz && mkdir -p $V
    cp mouth_closed.png $V/_base.png
    for m in mbp small mid wide round pucker teeth; do cp v_$m.png $V/$m.png; done
    cp eyes_closed.png $V/blink.png
    cp smile.png $V/smile.png
    cp surprised.png $V/surprised.png

Note `mouth_closed.png` becomes `_base.png`: the neutral closed mouth is what the kit
shows when it is not speaking, and every patch is a difference from *that*.

Then bake as above. The baker prints each patch's size and position, which is the quickest
sanity check there is:

```
02-nico_paz.png:
  mbp         60x42  at 394,312     7.4 KB
  wide        66x64  at 391,312    12.4 KB
  blink      126x62  at 361,232    22.9 KB
```

A mouth shape costs ~5–12 KB and **no decode at all** — the patch is RGB565A8, which is
LVGL's own layout, so the device points an image descriptor straight at flash. That is the
whole reason for the format: a re-encoded full frame costs 60 KB and a 180 ms decode, and
visemes change far faster than that decode could finish. Faces fill frames differently, so
a face that fills more of the shot costs proportionally more (the two shipped here differ
by 3–5×).

A photo with no `variants/` directory still works — it just uses the jaw puppet, or sits
still if it has no `mouths.csv` line either.

**Swiping needs the 16 KB main stack** (`sdkconfig.defaults`). LVGL draws on the main
task, and its image path — `lv_refr` → `lv_draw_image` → `lv_image_decoder_get_info`,
with an interrupt frame on top — measured ~8.9 KB on the board. At the old 6144 it ran off
the end of the stack into the heap beside it: corrupt TLSF headers and task lists, which
showed up as a reset on the second swipe and, once Wi-Fi was allocating in the damaged
region, as a screen of half-drawn frames stacked a few dozen pixels apart. The stack's
high-water mark never said so, because a deep frame does not have to write every byte it
skips over. Proven by moving LVGL to a 16 KB task: forty seconds of automatic swiping, no
corruption, 7.4 KB of the stack still untouched.
