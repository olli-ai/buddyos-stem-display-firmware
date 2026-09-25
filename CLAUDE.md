# CLAUDE.md

Guidance for Claude Code working in this repository.

## What this is

ESP-IDF (C) firmware for the **screen-equipped BuddyOS STEM device** — the kit that shows
the Buddy a child designed on the website. Panel driver, LVGL, a face renderer that
composites catalogue sprites, and an interpreter for the `.buddy` motion format.

Right now it is an **empty skeleton**. Read `README.md` before proposing work; the panel is
not chosen, and that decision gates the draw buffers, the sprite sizes and the partition
budget.

## The rule that matters most

**Do not modify `../buddyos-stem-c-firmware`.** That is the on-device *vision* spike —
camera, TFLite Micro, esp-nn — and it stays narrow on purpose. This device type does not
carry the offline image model; the 259 KB the vision stack costs is what makes a screen
affordable here. Read that repo for two things and change nothing:

- its **measurements** (what a device without the vision stack gets back);
- its **model loader** — the precedent for download-and-verify, which the face catalogue
  partition reuses.

## Where the design lives

Planning is in `../buddyos-stem-features-management` (documents, not code):

- `docs/features/kid-designed-faces-on-device-screen.md` — the whole feature and its phases.
- `docs/features/kid-authored-face-motion.md` — the timeline and the motion format.
- `docs/adr/0009-face-catalogue-as-updatable-partition.md` — why the artwork is its own
  partition, and the fallback rule when a part id is unknown.
- `docs/adr/0010-face-motion-as-keyframes-over-catalogue-layers.md` — the format decision,
  the byte layout, and why easing ships as a table rather than a formula.

A change here that contradicts those documents should change them too, in the same breath.

## The contract with the web

The format is defined once, in TypeScript, and this firmware follows it:

- `../buddyos-stem-fe/src/data/motion.ts` — layers, properties, ranges, easing table,
  sampling.
- `../buddyos-stem-fe/src/data/motion.fixtures.json` — **the conformance vectors**: motions
  paired with the exact values sampling must return. This firmware is correct when it
  reproduces them, and the per-easing `lutChecksum` is checked *first* — an implementation
  that reimplements a curve as a formula instead of reading the shipped table fails there,
  before a single sample is compared.
- `../buddyos-stem-fe/src/data/motion.spec.ts` — the same behaviour as runnable assertions.

Two rules follow from the byte layout and are easy to get wrong:

- A keyframe is **8 bytes**, padded. Seven would put every `u16` on an odd offset, and
  unaligned 16-bit loads on Xtensa fault or trap.
- A `sprite` track is **stepped, never interpolated**. Halfway between part 1 and part 8 is
  part 4, which is some other eye entirely.

## Conventions

- **Little-endian, and no byte-swapping** — it is what the chip already is.
- **Clamp values on read; never trust the file.** A design comes from a child and crosses a
  network. For a layer that is hygiene; when actuators arrive it is a safety requirement.
- **A design naming an unknown part degrades to a fallback**, never to a blank screen
  (ADR 0009). Test it with a deliberately stale catalogue.
- IDF **v5.2.2**, matching the org's Docker workflow. (The v5.5 breakage in the vision repo
  was an I²C `driver_ng` conflict from the *camera* driver, which this firmware does not
  have — so this pin is convention, not necessity.)
