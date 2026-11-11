# The face

How a face is baked, placed, animated and chosen — the renderer, the catalogue characters, the expressions a model picks and the skills a program can call.

[All docs](README.md) · [Repository README](../README.md)

---

### Re-baking the face for a panel

Both bakes are committed, so the firmware builds without the baker and without the
website repo: `main/generated/face_assets_720.c` for this board's JD9365 and
`face_assets_240.c` for the S3's GC9A01. `main/CMakeLists.txt` compiles exactly one,
chosen on `IDF_TARGET` — they define the same symbols. The art is vector, so a new size
re-rasterises rather than upscales. The cost is area, in ARGB8888: about 0.45 MB at 240
and 4.1 MB at 720, which is the whole reason for the 32 MB table above.

`tools/bake_face.ts` is deliberately untracked (it imports from `../buddyos-stem-fe` and
cannot compile without it), so if you have it, pick the size with `FACE_SIZE` and run:

    cd ../buddyos-stem-fe && npx esbuild ../buddyos-stem-display-firmware/tools/bake_face.ts \
        --bundle --platform=node --format=cjs --outfile=/tmp/bake.cjs \
        --loader:.webp=text --loader:.png=text \
    && FACE_SIZE=720 MASCOT_SRC=$PWD/src/assets/mascots/sprout.webp \
       node /tmp/bake.cjs ../buddyos-stem-display-firmware/main/generated

`MASCOT_SRC` is not optional, whatever the comment in the file says: the command above
bundles to CJS, where `import.meta.url` is `undefined`, so the relative path the baker
would otherwise derive throws `ERR_INVALID_URL`. Needs `rsvg-convert` and `magick`.

### The characters, and why they are baked at 384

`main/face_catalogue.c` reads the characters out of the `faces` partition and
memory-maps them, so they cost no RAM. The catalogue is committed as
**`faces/face_catalogue.bin`** — **Sprout, Pip and Leaf**, 1,506 KB — and the build
flashes it with everything else: it is in `flash_args`, so [Flash it](building.md#flash-it) puts
the characters on the kit with no extra step. One file serves both boards; each
resamples it to its own panel. The kit says what it found at boot —
`3 characters mapped from flash, 1506 KB, no RAM` — and names them.

A design that picks Boxy, Bolt or Chip on the website shows Sprout here and says so in
the log: those characters are whole objects rather than heads, and a child's chosen eyes
would land beside their real features.

The catalogue is baked from the website's artwork by `tools/bake_catalogue.ts`, which is
not in this repo: it imports the website's source directly. Rebaking is for when the
characters change; building never needs it.

To write the catalogue alone (after rebaking it, say), its offset is the `faces`
partition's — `0x912000` on the S3 N16R8 (`partitions-16mb.csv`), `0xd12000` on the P4
(`partitions-32mb.csv`) — and it moves if the table changes:

    esptool --chip esp32s3 -p PORT write-flash 0x912000 faces/face_catalogue.bin

**`SIZE` in that baker is 384, and that is forced by arithmetic.** A background is
RGB565A8 at SIZE², so at the panel's own 720 one character is 1,519 KB and three are
5,295 KB — 345% of a 1,536 KB partition. At 384 the three come to 1,506 KB, which is
98% of it. The firmware resamples each character up to the panel once, when it is
worn, so the cost is paid at a character change rather than per frame. More than
three characters, or a sharper one, needs the partition re-cut — which in turn needs
the 4.1 MB of baked artwork out of the app slot, since everything on this board must
stay under 16 MB.

### Drawing a face this panel can move

LVGL cannot animate this face, and that is structural rather than a tuning problem: its
hardware draw units are nxp, renesas and vg_lite — there is none for Espressif — so on
the P4 it is software by construction. Measured on the board, 720×720:

| | cost per frame |
| --- | --- |
| LVGL, scaling the body | 250–460 ms |
| LVGL, opaque full-screen blit, no scaling | 48–62 ms |
| **PPA, the same move** | **13–15 ms** |

So the body is composited by the **PPA** (`main/face_ppa.c`), straight into the frame
buffer LVGL is about to draw into, and LVGL draws only the features over it. During
motion a frame costs 23–33 ms, of which the body is 14–18 and the features the rest —
roughly 30–40 fps for a face moving as a whole.

Three things the hardware decides, all of which show up as product behaviour:

- **Translation is exact.** `PPA_SR_SCAL_X_FRAG` is four bits, so scale quantises to
  sixteenths and a breath of 1.025 rounds to nothing — but a nod or a lean is pixel
  accurate.
- **Rotation does not exist in hardware**, only quarter turns. The website's `shake` is
  a ±6° rotation, so the turn is rendered *before* the frame needs it: seven copies of
  the body at 2° steps, built the first time a design asks the head to turn (3.7 s and
  7 MB of PSRAM, once per character). An earlier attempt kept them at the 384 bake and
  let the PPA upscale — same information, cheaper blit, and visibly wrong: the PPA's
  scaler is cruder than a CPU bilinear pass and drew concentric rings across the body's
  gradient. The rotation and the upscale now happen in one CPU pass.
- **Whole screens, not dirty areas.** The body is what erases the previous frame, so a
  partial redraw would leave the last frame's eyes standing where the body had erased
  around them. `LV_DISPLAY_RENDER_MODE_FULL`; LVGL still renders only when something
  changed.

### What the face does on its own

- **A design previews when it arrives.** Press Save and the kit wears all seven states,
  about a second each, then returns. Only on a *change* — `face.update` is retained, so
  the broker replays it at every boot and a parade on every power-up would stop meaning
  anything.
- **Parts follow the character.** Drawn parts are authored for the built-in face, which
  is Sprout. On Pip a designed eye sat 14 px high and a designed mouth 57 px high, which
  is the mouth crowding up between the eyes. Each character now carries one offset per
  feature, measured when it is worn, so switching a state between `artwork` and a drawn
  part no longer moves the feature.
- **Baked loops come home to the design.** A sprite track names absolute sprites, and
  the face it was generated from is the factory's — so every time a blink or the 480 ms
  talking cycle came home it overwrote the child's eyes. Home now means "whatever the
  design says".
- **A design brings its own motion.** `extra.motion` is interpreted at runtime, per
  state and whole: re-author `speaking` and `idle` keeps the loop it was built with.
- **The face rests when the room is quiet** (`BUDDY_FACE_ASLEEP_S`, 45 s). Without it an
  idle state a child gave a lively loop would run for hours at nobody.

### Expressions, chosen by the model

omni's `visual_face_emotion` plugin can change the face mid-sentence. The kit advertises
what it can wear when the voice socket opens — a `face2d` context listing the preset ids
it has artwork for — and omni supplies the wording the model is given for each. A device
sends ids; it never sends prompt text.

An expression is a **second axis from state**: a Buddy can be `speaking` and `surprised`
at once. It takes the eyes and brows and leaves the mouth, because the mouth is the part
a child designs and letting the model overwrite it meant a designed speaking face lasted
only until the first tag. It is cleared at the start of the next turn, not at the next
state change — the expression arrives while omni is still `thinking`, so clearing on a
state change wiped it the moment the state became `speaking`.

Timing comes from the audio, not the text. omni reports `at_ms`, the tag's position in
the turn, and the kit holds each expression until *its own* playback reaches that point —
measured across one 51 s reply, every expression landed within 7–20 ms of its due point,
which is one Opus packet.

### Skills: what a program can ask this kit to do

A child's program is **not run on the kit**. omni injects it into the model's prompt and
the model decides when to act, then publishes an MQTT directive. The kit implements two
things a program may name:

| directive | effect |
| --- | --- |
| `face.state` | wear one of `idle · listening · thinking · speaking · busy · error · asleep` |
| `face.rest` | settle until spoken to again |

`face.rest` is what "bye bye" should mean here. It settles the face at the first lull —
not during `speaking`, since the goodbye should be said with a face on — and **ends the
voice session**, because resting the face alone left the socket open and the kit went on
answering anything said near it while looking asleep.

Deliberately **not** `device.sleep`, which this firmware also implements: `device.` is a
reserved prefix omni refuses from a program (`device.reset` factory-resets a kit), and on
this board deep sleep is undone only by the RESET button — the P4 wakes from an LP GPIO,
BOOT is GPIO 35, and the touch panel is polled with no interrupt line. A Buddy that
deep-slept on "bye bye" would need an adult to revive it.

The user-facing version of this list is a published guide in the gateway,
`display-kit-program-actions`.
