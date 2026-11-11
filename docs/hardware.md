# The board, and wiring your own

What is on the PCB we ship, and the same table read as instructions for a breadboard. `main/board_pins.h` is the source of truth; if it and this page disagree, the file is right.

[All docs](README.md) · [Repository README](../README.md)

---

## What is on the board

Common parts, laid out rather than breadboarded — there is nothing custom in here but
the PCB itself.

| Part | What it is | Notes |
| --- | --- | --- |
| MCU | **ESP32-S3-WROOM-1-N16R8** — 16 MB flash, 8 MB octal PSRAM | **Not the N8R2.** See below. |
| Display | **GC9A01** 1.28" 240×240 round TFT, SPI, on an FPC (DS501/DS502) | Backlight through Q501 on GPIO 13. No touch. |
| Microphone | **INMP441** I²S MEMS, on J602 | The same part the Robot Kit uses. |
| Amplifier | **MAX98357A** (U601) | `GAIN_SLOT` open: a fixed 9 dB. |
| Speaker | 4 Ω, 2–3 W, small | **No connector** — solders to TP504/TP505. |
| Also fitted | BTN1 (GPIO 0, also BOOT), **BTN2 (GPIO 1, the talk button)**, VBAT sense (GPIO 8) | A WS2812 footprint on GPIO 2 is **not** fitted. BTN2 is read by `main/voice.c`; see [voice.md](voice.md). |

**Wiring your own is supported.** Every part above is an off-the-shelf breakout, so a
module on a breadboard works on the same pins — the table in [Wiring](hardware.md#wiring) is for
both. Two differences to expect, neither of them a problem: set
`CONFIG_BUDDY_S3_PANEL_MIRROR_X=n`, because a breakout is not mounted reversed the way
this board's FPC is; and your speaker goes on the amplifier's `+`/`−` terminals rather
than soldered to TP504/TP505.

**The module is the N16R8.** The older **N8R2** (8 MB flash, 2 MB PSRAM) keeps `partitions.csv`,
whose app slots are 2 MB — and the image with voice and the face in it measures
**2,408,704 B** (2026-11-04), which is 115% of one. The link succeeds and the build then stops at the
partition check. The 16 MB table (`partitions-16mb.csv`) gives 4.5 MB slots.

It is worth being precise about *why*, because "2 MB" is the table and not the chip, and
the two are easy to confuse:

- **Flash is the real constraint, and it is a table, not a wall.** Two 2.94 MB slots plus
  `faces` and `model` do fit an 8 MB part (`0x2F0000` each, which leaves the image at 79%
  of a slot); drop `faces`, which this build bakes into the app and never reads, and the
  slots reach 3.8 MB. Nobody has cut that table or built against it. What you would be
  buying is a slot with a fifth of headroom instead of a half, on a device whose artwork
  rides *inside* the firmware — which is the trade `partitions-16mb.csv` exists to avoid.
- **PSRAM capacity is not the constraint.** Measured on the board, the whole firmware's
  dynamic PSRAM use is about **185 KB** (free goes 6,062,524 → 5,876,928 min-ever). 2 MB
  is ample. What does not fit is `CONFIG_SPIRAM_XIP_FROM_PSRAM` — the measured 2.22 MB of
  `.text`/`.rodata` it copies in is more than an N8R2 has in total — but that lives in
  `sdkconfig.defaults.n16r8` and the N8R2 path never turns it on. It was added for the
  480×480 RGB panel this kit no longer has.
- **PSRAM bandwidth is the unmeasured risk.** The R2 part is quad, half the octal R8's,
  and voice, TLS and LVGL's reads from flash share that bus. This is the one that would
  need a board rather than arithmetic.
- **Pins are identical either way.** Octal PSRAM takes GPIO 35–37; on this board those
  three are unconnected at the module (confirmed in the schematic netlist), so a quad
  part simply hands them back.

**The amplifier needs a real 5 V rail**, which this board gives it. Recorded because it
cost a week on the hand-wired board that came before: there the 5 V read 4 V under load,
the 3V3 regulator ran out of headroom, and the first thing to fail was the USB-serial
bridge — dropped bytes, re-enumeration, and a board that looked bricked when it was only
starved. If a board behaves strangely the moment audio starts, measure the rail before
reading any code.

### Wiring

`main/board_pins.h` is the source of truth — if this table and that file ever disagree,
the file is right.

**This table is both the board's wiring and the one to follow by hand.** On the board we
ship it is already routed; on a breadboard it is what to connect. The parts are common
breakouts either way — a GC9A01 module, an INMP441 and a MAX98357A — and the only build
change for a hand-wired one is `CONFIG_BUDDY_S3_PANEL_MIRROR_X=n`.

| Signal | GPIO | Part pin |
| --- | --- | --- |
| **GC9A01** SCLK | 12 | `SCL`, through R501 |
| MOSI | 11 | `SDA`, through R502 |
| DC | 16 | `DC` |
| CS | 17 | `CS` (R503 pull-up) |
| RESET | 18 | `RST` (R504 pull-up) |
| Backlight | 13 | Q501 gate, high lights it |
| **I2S0** speaker BCLK | 47 | MAX98357A `BCLK` |
| speaker WS | 48 | MAX98357A `LRC` |
| MAX98357A DIN | 21 | |
| MAX98357A SD | 14 | `SD_MODE`: high enables, low mutes |
| **I2S1** mic SCK | 38 | INMP441 `SCK` |
| mic WS | 39 | INMP441 `WS` |
| INMP441 SD | 40 | the mic's data *out*, an input to us |

Hardwired, not driven: the amplifier's `GAIN_SLOT` (open: 9 dB) and the mic's `L/R`
(GND, at J602 pin 6: left). The panel's `VCC` is 3V3; the amplifier's VIN is **5 V**.

R501 and R502 are drawn as 22 Ω and fitted as 100 Ω (measured 2026-10-28). That cost a
bring-up week before the glass turned out to be the fault, and the replacement panel
runs this bus at the full 40 MHz with the 100 Ω parts still on it — so it is a note, not
a limit, but check them before believing a timing figure.

**The amplifier and the microphone have their own clock pairs**, so they are two I²S
controllers (I2S0 out, I2S1 in), not one in full duplex. The backlight is a transistor,
held off through panel reset and lit on the first frame, so the controller's power-on RAM
noise is never seen. And there is **no speaker connector**: `OUTP`/`OUTN` end on test
points **TP504/TP505** and the speaker solders to those pads — a board with nothing on
them is silent with every register and every clock correct.

The board also **mounts the panel the other way round**: the driver, the init and
`invert_color` are stock, and the face still comes out mirrored left to right, so it is
the FPC and not the software. `main/gc9a01.c` undoes it with
`esp_lcd_panel_mirror(true, false)` — a mirror, not a rotation — under
`BUDDY_S3_PANEL_MIRROR_X`, which is on by default. **A GC9A01 breakout is mounted the
normal way and needs it off.** It is a board description rather than a preference: on
any given board exactly one of the two settings is right, and the wrong one is obvious
the moment a word appears on the glass.

The enclosure also holds that panel at an angle, and for two days the face was turned
to match: `BOARD_FACE_ROT_DEG` in `main/board_pins.h`. **It is 0 since 2026-11-05 — the
face is not turned**, on either board. Everything below describes what that line does
when it is not 0, because the machinery is still there and the enclosure is the thing
that decides.

At 0 the whole of it compiles out. The status pill, the clock and the setup screen go
back to their flex columns, `main/turned.c` compiles to nothing, and `face_view.c`
skips the pass that resolves an angle into the body's pixels — so the screens are the
ones that were there before any of this, and the measurements further down become the
cost of a setting nobody is paying for. One line turns it off; the same line turns it
back on.

The sign is the half that is easy to get wrong, in a way that looks right until you hold
the kit up: the number is the turn applied to the *face*, so it has to oppose the
enclosure's rather than match it. The first build said +30, and on the glass that reads
as a face leaning about twice as hard as the bare panel did — not as something subtly
off. A face that comes out worse than no correction at all is this sign, every time.

The magnitude came off the glass rather than off a drawing: −30 was still visibly
short, −45 was close, and it settled at −48 on 2026-11-03. Anyone putting a number back
here should record where it came from. Nothing else moves when it changes — the cost
below is the same at any angle.

It is **not** a display rotation, and could not have been. The GC9A01's MADCTL has MX,
MY and MV — eight combinations, which is quarter turns and flips — and LVGL's enum is
`LV_DISPLAY_ROTATION_0/90/180/270` (`lv_display.h`). Nothing in either path expresses
an angle off the quarter. Turning the frame buffer in software instead would mean a
second 240×240 RGB565
buffer (115,200 B) and 57,600 pixels resampled *per frame*, on a panel whose SPI link
already costs 240×240×16 bits ÷ 40 MHz ≈ 23 ms a frame. The P4 side of this repo
reached the same conclusion from the other direction: it pre-renders thirteen fixed
angles into PSRAM precisely because turning the body per frame is not affordable even
with a PPA (`main/face_view.c`).

So the face is turned instead of the screen, in the two places that were already
turning things:

- **The body** is one flattened RGB565 image, rebuilt only when a child changes
  character. `render_rotated()` — the P4's resampler, now shared — does the turn in the
  same pass that resamples it, into the same buffer. **No extra memory and no per-frame
  cost at all.**
- **The features** (brows, eyes, mouth) are `lv_image`s, and `place()` already composed
  a head rotation into each one. The glass's angle is applied last, after the head's
  own move, so a head that nods still nods along the *face's* axis. An `lv_image` is
  the one thing LVGL transforms without an intermediate layer, which is what keeps this
  inside the memory rule the rest of the renderer is built around.

- **The voice status pill** turns too, by the same trick one step further. A pill is a
  container with a label in it, and LVGL would draw that through a layer — so on a
  turned board `main/status.c` rasterises the pill into an `lv_canvas` instead (the
  background, the rounded ends and the glyphs), and a canvas *is* an `lv_image`. The
  caption is redrawn only when the status changes, into one PSRAM buffer sized at
  startup for the longest of the seven. The coloured dot stays outside it, a leaf of its
  own placed at the turned position: a filled circle looks the same at every angle, and
  leaving it out keeps its pulse an opacity write on a leaf rather than a
  re-rasterisation of the text at animation rate.

The glass is round (`BOARD_ROUND`), so the turn uncovers no corner at any angle;
`render_rotated` fills what comes in from outside with black, and the bezel covers it.

**The pill's own numbers, rev A, 2026-11-03**, at −48°: the canvas is **186 × 34, 25,296 B
of PSRAM**, allocated once at startup, out of 5.85 MB free — and the LVGL pool went
*down*, to **11,732 of 30,616 used, peak 13,912, 7% fragmented**, from 11,848 and a peak
of 14,088 at −45° with the flex pill. It should: the label object and the text buffer it
kept in the pool are gone, and what replaced them lives in PSRAM. Frame means were
**276–2,111 µs**.

> **Proven on the glass on 2026-11-04.** The run that established the figures above was
> one where the kit never associated (`no access point with that SSID in range`), and
> `voice_start()` is what first calls `status_set()` — so the pill was allocated and
> placed but never drew a caption. With the AP back in range it does: `[wifi connected]`
> at 12,767 ms and `voice: listening for the wake word` at 12,906 ms, which is
> `status_set()` reached and the caption rasterised. The canvas is the same 186 × 34.

**Measured on rev A, 2026-11-03**, 60 s from reset, over several angles. The cost that
was worth worrying about did not appear: at rest the five features used to take LVGL's
plain blit path and now always take the transformed one, which asks `lv_malloc` for up
to `MAX_BUF_SIZE` per draw — and that is **4 × 240 × 2 = 1,920 B**, sized off the
*display's* RGB565 rather than off the source, so the ARGB8888 pill asks for no more
than a face sprite does. The pool barely noticed: **12,004 of 30,572 used, peak 14,184**
at +30°, **12,016 of 30,564, peak 14,196** at −30°, **11,848 of 30,596, peak 14,088** at
−45°, against 11,916 and a peak near 14,000 before any turn — noise, in other words, and
the same noise at every angle. The transformed-draw buffer is transient and the peak
already carried it, because any design with a rotation track was taking that path
anyway. Frame time stayed well inside budget too: means of **222–4,053 µs** per 301
frames against the 16,000 µs that is 60 fps. The worst-case spikes in the same logs
(10–231 ms) line up with Wi-Fi association retries, not with the face — though that is a
correlation, not a before-and-after.

**The clock and the setup screen turn too**, since 2026-11-04, by the pill's trick
factored out of it. `main/turned.c` is a block of centred lines rasterised into one
PSRAM canvas and rotated as an `lv_image`; `turned_point()` carries a position from the
upright layout to the turned one, so each block is *placed* rather than laid out, and
flex is gone from both screens on a turned board. The whole file compiles to nothing
where `BOARD_FACE_ROT_DEG` is 0 — which, since 2026-11-05, is both boards. The flex columns
are back on the S3 too; the turned path is kept for the next enclosure that needs it.

Three things that layout had to resolve:

- **The clock is two blocks, not one.** The seconds change every tick and the date once
  a day, so splitting them means one small re-rasterisation a second rather than a large
  one. The full-screen black backdrop stays an ordinary rectangle: a black circle looks
  the same at every angle.
- **The QR needs a canvas of its own, for two reasons.** `lv_qrcode`'s buffer is
  `LV_COLOR_FORMAT_I1`, and the software renderer's transform has no indexed case at all
  (`lv_draw_sw_transform.c` handles RGB565/RGB888/XRGB8888/ARGB8888/A8) — rotating the
  widget would draw nothing. And its white quiet zone is an object *border*, drawn square
  around the widget's box whatever the image inside does, so a rotated code would push
  its corners out onto black. So the I1 bitmap is drawn untransformed into an ARGB8888
  canvas that carries the quiet zone as pixels, once, and that canvas is what turns.
- **That first rasterisation is deferred to the LVGL timer.** `lv_canvas_finish_layer()`
  dispatches and waits on the draw unit, which is not a thing to do from create-time code
  before the first `lv_timer_handler()`. `qr_once()` is called from the setup screen's
  100 ms tick and guarded by a `static bool`.

No PSRAM for a block means the block does not appear — `turned_create()` says so in the
log and every later call is a no-op. Degraded, never blank and never a hang; and for the
QR, the printed serial is still the way in.

**What still does not turn** — the volume panel. It stays upright over a tilted face.
The way to make it follow is `turned.c`'s, not a transform on the container: LVGL draws
*any* transformed object through an intermediate layer except `lv_image`, and a 240×240
layer is 115 KB, more than the whole LVGL heap (the rev A bring-up below is what running
out of that pool looks like).

**Measured on rev A, 2026-11-04**, at −48°, from `run-buddy.log`:

| block | canvas | PSRAM |
| --- | --- | --- |
| status pill | 186 × 34 | 25,296 B |
| clock, time | 240 × 44 | 42,240 B |
| clock, date | 240 × 50 | 48,000 B |
| setup, title + serial | 192 × 74 | 56,832 B |
| setup, step | 192 × 48 | 36,864 B |
| setup, hint | 192 × 48 | 36,864 B |
| setup, QR + quiet zone | 102 × 102 | 41,616 B |

All of it allocated once at startup, out of ~6.06 MB free at boot: 5,659,004 B free at
`[gallery up]`, and **5,059,336 B steady while running, unchanged second to second** —
which is the claim that mattered, because a per-tick allocation is what re-rasterising
text once a second could have cost and does not. The LVGL pool went *down* again, to
**10,572 of 30,844 used, peak 12,664, 11% fragmented** at `[gallery up]`, from 11,732 and
a peak of 13,912 with the flex pill: labels and their text buffers left the pool and
their pixels live in PSRAM. With the clock up it is **11,280 of 30,724, peak 13,444, 7%**.

Frame time is the one place the clock is visibly more expensive, and only in the worst
case: with the face showing, means of **594–718 µs** and a worst frame near **13 ms**;
with the clock up, means of **241–346 µs** — the face stops animating under an opaque
screen — and worst frames of **22.4–27.3 ms**, 35.3 ms in the first window after it
appeared. So the worst frame roughly doubles, once a second, while the mean halves. The
log cannot separate the rasterising from the full-screen flush that follows it.

> **The clock was seen on the glass** in that run (`I (182554) clock: left alone; showing
> the time`, after the 180 s `CONFIG_BUDDY_CLOCK_IDLE_S`). **The setup screen was not.**
> Reaching it means holding BOOT for 2 s, which erases the kit's Wi-Fi credentials, and
> that was not worth doing to look at it. Everything on it is allocated and logged at
> startup, so what is unproven is the drawing: the three text blocks, and `qr_once()`'s
> I1-into-ARGB8888 blit in particular. If that blit does not work the QR is a blank white
> square, not a crash — and the serial is printed above it either way.

GPIO 0 is BOOT and nothing else, so it works as a button at any time: hold it for 2 s
while the kit runs and let go to set the kit up again. The strapping pins 3, 45 and 46,
the octal PSRAM (35–37), native USB (19/20) and flash (26–32) are all left alone.
