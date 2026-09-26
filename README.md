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

## Building

Dependencies are **vendored in `components/`** and the component manager is switched off.
idf 5.2.2's resolver cannot parse a `&&` in a dependency manifest and fails while merely
*listing* registry versions, so pinning cannot avoid it — but nothing here needs the
registry:

    docker run --rm -e IDF_COMPONENT_MANAGER=0 -v "$PWD":/fw -w /fw \
        espressif/idf:v5.2.2 idf.py -DIDF_TARGET=esp32s3 build

`components/lvgl` is a shallow clone of LVGL 9.2 (cloned by `sim/build.sh` on first run,
shared by both builds). `components/esp_lcd_gc9a01` is copied from esp-bsp with its two
esp-bsp cmake-helper lines commented out and the version triple supplied directly.

The panel driver glue is written here rather than pulled from `esp_lvgl_port`: a display,
two DMA-capable partial buffers, a flush that waits on the panel's completion callback,
and a tick from `esp_timer`. About sixty lines, and one fewer dependency.

## Watching it without hardware

`sim/` builds the same `face_view.c` / `face_motion.c` / `face_assets.c` for the host and
renders them into a memory buffer. See **Simulator** below.

## Status

**It builds, and it has never run on a panel.** As of 2026-09-26, for ESP32-S3:

| | |
| --- | --- |
| Static IRAM | 68,582 B (18.9% used) |
| Static D/IRAM | 79,576 B — `.data` 11,216 + `.bss` 68,360 (23.0% used, ~266 KB free) |
| Flash | 589,875 B — `.text` 340,211 + `.rodata` 249,408 |
| App image | 669,673 B, 68% of the partition free |

`.bss` already contains LVGL's 62 KB heap, and the two DMA draw buffers (23,040 B) are
allocated at boot on top — so roughly **103 KB of internal RAM**, leaving ~240 KB for the
radio stack this firmware does not yet bring up. That last clause is the whole caveat: the
planning docs' gate is free internal SRAM **with Wi-Fi and MQTT running**, and that number
does not exist yet.

## Simulator

    docker run --rm -p 8080:8080 -v "$PWD":/fw -w /fw espressif/idf:v5.2.2 \
        bash sim/build.sh --serve

Then open <http://localhost:8080/>. It uses the container's *host* gcc, not the Xtensa
toolchain, so none of the IDF-version trouble applies. LVGL is cached as a static library
after the first build. Without `--serve` it writes one PPM per sampled moment instead —
`docs/sim-states.png` is made that way.

It runs the same two partial draw buffers `main.c` gives the device, and reports what a
face costs: draw buffers, LVGL heap used and peak, fragmentation, and the sprite bytes
that live in flash rather than RAM. What it cannot see: the internal-versus-PSRAM split,
DMA-capable memory, a 240 MHz Xtensa doing the blending, and the radio taking cycles.

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
