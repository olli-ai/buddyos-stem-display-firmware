# buddyos-stem-display-firmware

ESP-IDF firmware for the **screen-equipped BuddyOS STEM device**: the kit that shows the
Buddy a child designed on the website, drawn with LVGL and animated from the child's own
keyframes.

It is planned in `../buddyos-stem-features-management`:

- [Kid-designed Buddy faces on a device screen](../buddyos-stem-features-management/docs/features/kid-designed-faces-on-device-screen.md)
- [Kid-authored face motion](../buddyos-stem-features-management/docs/features/kid-authored-face-motion.md)
- [ADR 0009 — the face catalogue ships as its own updatable partition](../buddyos-stem-features-management/docs/adr/0009-face-catalogue-as-updatable-partition.md)
- [ADR 0010 — motion is keyframes over targets](../buddyos-stem-features-management/docs/adr/0010-face-motion-as-keyframes-over-catalogue-layers.md)

## What this repo is for

1. **Panel bring-up** — driver, LVGL, a frame on the glass.
2. **The face renderer** — composite catalogue sprites into a face from a few hundred bytes
   of the child's choices. Never pixels over the wire; part ids and colours.
3. **The `.buddy` motion interpreter** — walk keyframe tracks, drive `lv_anim`. The format
   is defined in `../buddyos-stem-fe/src/data/motion.ts`, and its behaviour is pinned by
   `src/data/motion.fixtures.json` there. **Those fixtures are the contract**: this firmware
   is correct when it reproduces them, including the per-easing LUT checksums.
4. **The catalogue partition** — artwork delivered and updated without a firmware release,
   over the download-and-verify path the vision model already proves.

## What this repo is *not*

**It is not `../buddyos-stem-c-firmware`.** That repo is the on-device *vision* spike —
camera, TFLite Micro, esp-nn — deliberately narrow, and it stays that way. This device type
**does not carry the offline image model**, which is the entire premise of the faces spec:
the vision stack is 259 KB of a 1,449 KB app image, and the memory it frees is what makes a
screen affordable at all.

Read that repo for two things only, and do not change it:
- its **measurements** — what a device without the vision stack gets back;
- its **model loader**, the precedent for download-and-verify (presigned URL → streamed to
  flash → CRC checked before commit).

## Status

**Empty skeleton.** Nothing here drives hardware yet, and the most important input is still
missing — see below.

## Open questions blocking real work

- **Which panel?** Resolution, bus and shape gate the art, the draw buffers and the
  catalogue budget. A round 240×240 on SPI is the natural guess for a character face and is
  cheap on every axis, but it is **unconfirmed**, and `partitions.csv` and the LVGL buffer
  sizing here are provisional until it is.
- **Which SoC and how much flash?** The partition table below assumes 8 MB. It is a
  placeholder.
- **IDF version.** Pinned to v5.2.2 to match the org's existing Docker workflow. Note the
  v5.5 problem in `../buddyos-stem-c-firmware` was an I²C `driver_ng` conflict caused by the
  *camera* driver, which this firmware does not have — so that constraint is inherited by
  convention here, not by necessity.
