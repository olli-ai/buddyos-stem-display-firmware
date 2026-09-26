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
5. **Lip-sync** — the mouth follows the audio the Buddy is speaking, never its text. See
   **Lip-sync** below.

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

**It builds, and it has never run on a panel.** As of 2026-09-26, ESP32-S3 with Wi-Fi and
MQTT linked in, PSRAM enabled:

| | face only | with Wi-Fi + MQTT | + lip-sync |
| --- | --- | --- | --- |
| Static IRAM | 68,582 B | 94,566 B (26.1%) | 94,566 B |
| Static D/IRAM | 46,808 B | 68,512 B (19.8%, **277,344 B free**) | 75,736 B (21.9%, **270,120 B free**) |
| App image | 669,673 B | 1,332,841 B (of a 2 MB partition) | 1,432,989 B |

The lip-sync column is the demo build (`CONFIG_BUDDY_LIPSYNC_DEMO`), so its flash figure
carries 76,800 B of embedded speech that a product build would not. What the classifier
itself costs is **2,094 B of code and 7,204 B of RAM** — three 512-float arrays and the
sample window, static rather than heap so the cost is visible here instead of arriving
during a call. Halving `FRAME` would halve the RAM and the arithmetic; it would also halve
the frequency resolution, which is what separates /i/ from /u/, so it is not free.

RAM cost of the radio, by archive: `libpp` 13,942 B, `libnet80211` 11,997 B, `liblwip`
3,721 B, `libesp_wifi` 862 B, `libmbedtls` 266 B, `libmqtt` 0 B (all flash).

Take the two DMA draw buffers (23,040 B) off the 277,344 B and roughly **254 KB of internal
SRAM is free at boot**. That is *before* Wi-Fi's dynamic allocations, which are the large
ones and which `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` sends to PSRAM. **The number the
planning docs ask for is the runtime one, and it only exists on hardware.**

## Taking the measurement

    idf.py menuconfig          # BuddyOS display firmware -> SSID, password, broker
    idf.py -p /dev/ttyUSB0 flash monitor

Every milestone logs one `mem` line — `boot`, `face up`, `wifi init`, `wifi started`,
`wifi connected`, `mqtt started`, `mqtt connected`, then `running` periodically:

    I (mem) [mqtt connected] internal: free N, min-ever N, largest N | dma: free N, largest N
    I (mem) [mqtt connected] psram:    free N, min-ever N
    I (mem) [mqtt connected] lvgl:     used N of N, peak N, frag N%

**`min-ever` is the figure that matters** — the low-water mark, not whatever happens to be
free when you look. And `largest` next to a healthy `free` is how fragmentation announces
itself before an allocation fails. With no SSID configured the Wi-Fi stack still comes up
and reports; it simply does not associate, which is itself a useful baseline.

Worth running twice, with `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` on and off: on is the
answer for this board, off is the pessimistic case and tells you how much of the margin is
really PSRAM's doing.

## Lip-sync

The mouth shape comes from **the audio, never the text**. This product speaks Vietnamese
and English and switches between them inside a sentence; text would mean grapheme-to-
phoneme rules per language, while a vowel's formants are the same in either and a
fricative is noise in both. `main/face_lipsync.c` is a transcription of
`../buddyos-stem-fe/src/data/visemes.ts`, and the thresholds are **generated** into
`main/generated/face_assets.c` by the baker rather than copied — the same discipline as the
easing table.

Two rules in that file carry most of the weight, and both were learned the expensive way:

- **Loudness is relative.** Every persona has a cloned voice at whatever level the child
  recorded, so a fixed gate leaves one Buddy mute and another chewing on room noise.
- **A fricative is noise, not brightness.** "Energy above 4 kHz" fires on half of a bright
  voice's vowels. Zero-crossing rate and spectral flatness ask the right question.

`face_view_set_live_mouth()` overrides **the mouth only** — brows, eyes and the head keep
the motion the child authored, so a Buddy that blinks and tilts while it talks goes on
doing both.

Feed it the PCM on its way to the speaker, never the microphone. A device with both would
otherwise lip-sync to the room, and to itself.

### Holding it to the website's answer

    docker run --rm -v "$PWD":/fw -w /fw espressif/idf:v5.2.2 bash tools/xcheck.sh

Two generated goldens, both emitted by `tools/bake_face.ts` from the front end's own
TypeScript: `xcheck.txt` for motion sampling, `lipsync-xcheck.txt` for the classifier over
`tools/lipsync-fixture.pcm` — 2.4 s of real Omnivoice speech, Vietnamese then English in
one utterance, because code-switching is the case the design exists for. The C is fed in
173-sample chunks that divide neither the frame nor the hop, since the decoder will not
hand over tidy buffers either.

Currently **149 of 149 frames agree**. If the two sides ever diverge, the same face
lip-syncs differently in the studio and on the kit, and a child is the one who finds it.

![lip-sync frames](docs/lipsync-strip.png)

Those are simulator frames, 96 ms apart, through the fixture. Rendering them is not
decoration: an earlier bug had two classes drawing the *same* mouth, which every number in
the harness called healthy and only a picture showed.

### What is not measured

**CPU.** A 512-point FFT and a spectral flatness every 16 ms of audio is the one part of
this that could plausibly not fit, and it has never run on silicon. The demo build logs
`lip-sync: N us per frame` every hundred frames — that line is the measurement, and it
needs a panel and a board to exist.

## Simulator

    docker run --rm -p 8080:8080 -v "$PWD":/fw -w /fw espressif/idf:v5.2.2 \
        bash sim/build.sh --serve

Then open <http://localhost:8080/>. It uses the container's *host* gcc, not the Xtensa
toolchain, so none of the IDF-version trouble applies. LVGL is cached as a static library
after the first build. Without `--serve` it writes one PPM per sampled moment instead —
`docs/sim-states.png` is made that way.

It plays `tools/lipsync-fixture.pcm` through the classifier while the face is in its
speaking state, with the audio clock tied to the sim clock — so a paused frame shows a real
mouth shape for a real instant of speech. `FACE_PCM=<file>` plays something else.

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
