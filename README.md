# buddyos-stem-display-firmware

ESP-IDF firmware for the **screen-equipped BuddyOS STEM device**: a round screen showing
the Buddy a child designed on the website, and behind it the same kit as ever — BLE
Wi-Fi setup, MQTT to the BuddyOS gateway, and a voice conversation that starts when you
say "Jarvis".

## Which board

Two boards carry this device type, and **one branch builds both**. There is no flag to
set: the target decides, and `main/CMakeLists.txt` picks the matching face artwork.

| Your board | Target | Screen |
| --- | --- | --- |
| **ESP32-S3-WROOM-1-N16R8** (16 MB flash, 8 MB PSRAM) | `esp32s3` | HD21001C40 2.1" 480x480 round ST7701S |
| **ESP32-P4** (32 MB flash), e.g. Waveshare ESP32-P4-WIFI6-Touch-LCD-4C | `esp32p4` | 4" 720x720 JD9365 MIPI-DSI, touch |

**Both build with ESP-IDF v5.5.3.** Older IDF does not build this tree (the P4's panel
driver needs 5.4 or later). No IDF installed? Use Espressif's image,
`espressif/idf:v5.5.3` — see [Building](#building).

```sh
git clone https://github.com/olli-ai/buddyos-stem-display-firmware.git
cd buddyos-stem-display-firmware

# LVGL is not committed — clone it once, before the first build
git clone --depth 1 --branch release/v9.2 https://github.com/lvgl/lvgl.git components/lvgl

# ESP32-S3-WROOM-1-N16R8 — the N16R8 overlay is not applied automatically
idf.py -B build-s3 -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-s3/sdkconfig \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.n16r8" build

# ESP32-P4 — IDF layers sdkconfig.defaults.esp32p4 on its own
idf.py -B build-p4 -DIDF_TARGET=esp32p4 -DSDKCONFIG=build-p4/sdkconfig build
```

**Voice is opt-in, and the default build has none.** `BUDDY_AUDIO` is off because
bringing the audio board up the first time once hung a device past the ROM bootloader
(`main/Kconfig.projbuild` keeps the note), and `BUDDY_VOICE` hangs off it. A kit that
should answer to "Jarvis" needs both, which is two lines in `menuconfig` or appended to
the board's defaults:

```
CONFIG_BUDDY_AUDIO=y
CONFIG_BUDDY_VOICE=y
```

Everything downstream of them already defaults to what the bench measured — echo
cancellation on for the P4, and 30 dB of uplink gain, without which the cloud's VAD
never hears a voice and the kit cannot be interrupted.

**Do not build the N16R8 in the same directory as the P4**, or either in the default
`build/`: a partition table and a face bake are per board, and a stale `sdkconfig` is
how you get an image for the panel you do not have. The `-B` and `-DSDKCONFIG` above
keep them apart.

**Switching target also means deleting `dependencies.lock`.** It sits at the project
root rather than in the build directory, it is gitignored, and it belongs to whichever
target resolved last — so the first build after a switch fails while resolving a
component for the other chip. `rm dependencies.lock` and build again; nothing is lost,
it is regenerated.

**What is per board, and why.** The face is vector art rasterised offline, so its cost is
area and each panel needs its own bake — 480x480 for the S3 (~1.7 MB), 720x720 for the P4
(~4 MB). Both are committed and exactly one is compiled, which is why building needs
nothing but ESP-IDF. `face_view.c` takes the bake's dimensions *as* the panel's, so a
mismatched one does not scale to fit; it draws short of the edges. The P4 additionally
compiles `face_ppa.c`, its 2D accelerator, and its own panel and audio drivers.

The two boards took separate branches for a while, when the P4's bake was the only one
and it did not fit the S3's app slot. They no longer do, and fixes land once.

## Setting your kit up

Once the firmware is flashed, everything else happens at **<https://app.buddystem.com>** —
create an account, add the kit over Bluetooth (the setup screen shows its serial as a QR
to scan), design its face, give it a voice and choose what a touch does. A kit you built
yourself appears there exactly like one that came in a box.

---

**The face is the default build** (`BUDDY_SCREEN_FACE`). A design saved on the website
arrives over MQTT, is applied to all seven states and survives a reboot; the character
comes from the catalogue partition; the mouth follows the audio being spoken; and the
model can change the expression mid-sentence. The **photo gallery** — footballers you
swipe between — is the other half of the same firmware and is chosen with
`BUDDY_SCREEN_GALLERY` in `menuconfig`. The two are exclusive: one screen, one job.

**You can build one.** The parts are off-the-shelf modules on a breadboard, this firmware is
Apache-2.0, and a kit you flashed yourself joins the BuddyOS cloud exactly like one that came
in a box — same provisioning, same voice. See **[Build one yourself](#build-one-yourself)**.

> **This README is an engineering record**, not a tutorial. Every measurement here was
> taken on real hardware. If you only want to get a kit working, read [Build one
> yourself](#build-one-yourself), [The gallery](#the-gallery) and [Setting a kit
> up](#setting-a-kit-up), then stop.

## What this repo is for

1. **Panel bring-up** — the ST7701S RGB panel, LVGL, the capacitive touch layer.
2. **The gallery** — photos baked into the app, decoded on demand into PSRAM, one swipe
   apart. See [The gallery](#the-gallery).
3. **The kit around it, unchanged** — BLE Wi-Fi setup, MQTT to the gateway, the wake word
   and the voice path, and what it does when nobody is there.

## What this repo is *not*

**It is a gallery, a voice and a screen — nothing else.** The only inference on the device
is the wake word (ESP-SR WakeNet, in its own partition). Anything heavier does not belong
beside the voice path, and the measurements in this README are the argument.

## Build one yourself

Everything here is a common module and a breadboard. No PCB, no reflow, no custom parts.

| Part | What to get | Notes |
| --- | --- | --- |
| MCU | **ESP32-S3-WROOM-1-N16R8** devkit — 16 MB flash, 8 MB PSRAM | **Not the N8R2.** See below. |
| Display | **HD21001C40** 2.1" 480×480 round IPS, ST7701S, 16-bit RGB + SPI init | Backlight hardwired on. Needs the octal-PSRAM N16R8 for its frame buffer. |
| Touch | **HCY-CTP00021-A0** capacitive, I²C | Shares its reset with the panel. |
| Microphone | **INMP441** I²S MEMS breakout | The same part the Robot Kit uses. |
| Amplifier | **MAX98357A** I²S breakout | |
| Speaker | 4 Ω, 2–3 W, small | |
| Power | A **5 V supply for the amplifier**, not the board's 3V3 | Not optional — read the warning below. |

**Buy the N16R8.** The older **N8R2** (8 MB flash, 2 MB PSRAM) cannot drive this panel: an
RGB panel is refreshed from a 460 KB frame buffer in PSRAM, continuously, and quad PSRAM
at 80 MHz does not have the bandwidth to spare. Its 2 MB app slots also cannot hold the
voice path *and* BLE setup together; the build fails at link time.
The 16 MB table (`partitions-16mb.csv`) gives 3 MB slots and the app lands at 1.50 MB.

**Power the amplifier from 5 V.** On the first bench board the 5 V rail read 4 V under load,
the 3V3 regulator ran out of headroom, and the first thing to fail was the USB-serial bridge:
dropped bytes, re-enumeration, and a board that looked bricked when it was only starved. If
your board behaves strangely the moment audio starts, this is why.

### Wiring

`main/board_pins.h` is the source of truth — if this table and that file ever disagree,
the file is right. As wired on the N16R8 bench board with the HD21001C40 panel:

| Signal | GPIO | Panel / module pin |
| --- | --- | --- |
| **ST7701S** SPI CS | 42 | `CS`, 10 k pull-up to 3V3 |
| SPI SCK | 1 | `SCK` — shared with DB1 |
| SPI SDA | 2 | `SDA` — shared with DB2 |
| RESET | 41 | `RESET` **and** `TP_RST` |
| PCLK | 21 | |
| DE | 38 | |
| VSYNC | 39 | |
| HSYNC | 40 | |
| DB1–DB5 (blue) | 1, 2, 3, 8, 9 | DB0 to GND |
| DB6–DB11 (green) | 10, 11, 12, 13, 14, 15 | |
| DB13–DB17 (red) | 16, 17, 18, 46, 0 | DB12 to GND |
| **HCY-CTP00021-A0** SDA | 47 | 4.7 k pull-up |
| SCL | 48 | 4.7 k pull-up |
| **I²S** BCLK | 5 | MAX98357A `BCLK` **and** INMP441 `SCK` |
| WS | 6 | MAX98357A `LRC` **and** INMP441 `WS` |
| MAX98357A DIN | 7 | |
| INMP441 SD | 4 | the mic's data *out*, an input to us |

Hardwired, not driven: the panel backlight (LEDA to 5 V, LEDK1/2 to GND through a
resistor each — **it cannot be dimmed or switched off**; sleep blanks the panel with
DISPOFF/SLPIN instead), the amplifier's `SD` (100 k to VIN: on, left channel), its `GAIN`
(floating: 9 dB) and the mic's `L/R` (GND: left). VCI, IOVCC and TP_VCI are 3V3; the
amplifier's VIN is **5 V**.

The amp and the mic share BCLK and WS, so they are one I²S controller in full duplex
(`main/audio.c`), not two.

This panel takes almost every pin the S3 has. Three consequences, all deliberate:

- **GPIO 0, 3 and 46 are strapping pins and carry panel data.** That is why DB0 and DB12 —
  the least significant bits of blue and red — are tied to GND rather than given pins.
  The panel is idle at reset, so the straps read their pull resistors as usual.
- **BOOT (GPIO 0) is DB17.** While the panel runs it is an LCD output and cannot be read.
  It still wakes the kit from sleep (the panel is dark by then) and is read once at boot
  for the factory reset — see [Going to sleep](#deep-sleep-and-why-idle-no-longer-reaches-it). **Do not
  press BOOT while the screen is on:** it shorts a driven output to GND. Harmless for
  a moment, and the top bit of red flickers, but it is not a button while awake.
- **SCK and SDA double as DB1 and DB2.** The ST7701S is set up over SPI before the RGB
  bus starts; `st7701_set_on()` borrows the two pins back for DISPOFF/SLPIN with CS held
  high the rest of the time, so the panel ignores the pixel data crossing them.

The GC9A01 bench board before this one used SPI on 15–18/8, a muted, gain-switched amp on
9–13 and the mic on 4–7. Those numbers are in older notes and will not work with this build.

### Flash it

Build first ([Building](#building)) — one build directory per board, `build-s3` or
`build-p4`. Then, on the machine the board is plugged into:

**1. Install esptool 5.x.** Once, in its own environment:

    python3 -m venv venv && venv/bin/pip install esptool

Use this rather than the esptool bundled with ESP-IDF 5.5.3, which is 4.12: on the P4 its
flasher stub cannot verify the flash chip, and the write fails. A Docker build has no
esptool and the container cannot see a USB port anyway, so flashing always happens on the
host. **`esptool` and `write-flash`, not `esptool.py` and `write_flash`** — esptool 5
deprecated the older spellings.

**2. Find the port, and check the chip on it.** On macOS it is a `/dev/cu.usbserial-*` or
`/dev/cu.usbmodem*` (`ls /dev/cu.*`), on Linux `/dev/ttyUSB*` or `/dev/ttyACM*`, on Windows
a `COM` number. On Linux, *permission denied* means your account is not in the `dialout`
group. Before writing anything:

    venv/bin/esptool -p PORT chip-id

It must name the chip you built for — ESP32-S3 or ESP32-P4. A port name says nothing about
which board is behind it.

**3. Write everything the build produced**, from inside the build directory:

    cd build-s3
    ../venv/bin/esptool --chip esp32s3 -p PORT write-flash "@flash_args"

For the P4, the same from `build-p4` with `--chip esp32p4`.

- **Use `@flash_args`, not offsets copied from anywhere.** The build writes that file with
  every image, its offset and the flash settings, for exactly what you just built.
  Offsets move whenever the partition table does, and the two boards differ — the P4's
  bootloader goes at `0x2000`, the S3's at `0x0`. An image at the wrong offset looks
  exactly like a dead board.
- **Write the whole set, not just the app.** It includes the bootloader, which a board that
  has never run this firmware cannot start without, and `ota_data_initial.bin`, which
  points the boot at the image you just wrote. Write only the app to a kit that once
  updated itself over the air, and it keeps booting the old one.
- **Leave the baud rate alone on a native USB port** (`usbmodem` / `ttyACM`, as on the P4).
  Asking for 460800 there breaks the connection mid-flash.

**4. If esptool cannot connect**, put the chip into its download mode by hand: hold
**BOOT**, press and release **RESET**, then release BOOT, and run the command again. On the
S3 board BOOT is also one of the panel's data lines, so treat it as a button only for this,
and only while the screen is dark.

**What a flash keeps.** `write-flash "@flash_args"` does not touch the `nvs` partition, where
the kit keeps its Wi-Fi and account setup, or `faces`, where the character catalogue lives
([The characters](#the-characters-and-why-they-are-baked-at-384)). A reflashed kit comes back
already set up. **`esptool erase-flash` removes both**: the kit returns to setup, and shows
only the one character built into the firmware until the catalogue is written again. To
clear just the setup, hold BOOT for 5 s instead ([Setting a kit up](#setting-a-kit-up)).

**5. Watch it boot.** Opening the serial port resets the board, so a monitor always shows
the boot from the start:

    idf.py -B build-s3 -p PORT monitor

(any serial terminal at 115200 works too; `idf.py monitor` also decodes a crash backtrace
against the firmware you built). In order, a healthy kit logs:

- `N characters mapped from flash` — the catalogue was found (absent on a never-catalogued kit,
  which shows its built-in character);
- `mqtt connected` — set up and on the network;
- `wake word model …` — only in a build with voice on;
- after three quiet minutes, `left alone; showing the time`; then on "Jarvis",
  `wake word: opening a conversation` and `back to the face`.

With nothing else done, the kit shows the Buddy's face. To put it on the network, carry on
to [Setting a kit up](#setting-a-kit-up).

### Bringing it onto BuddyOS

The firmware is free; the cloud it talks to is a paid service, and that is the deal. Register
at the BuddyOS STEM website, then follow the **display-kit setup guide** in its *Documents*
section — it is written for people holding a board, kept in step with this repo, and available
in English and Vietnamese. It covers creating the account, provisioning over Bluetooth
and what cloud usage costs. (Its face-designer pages do not apply: this build shows no
face, so a design saved in the studio arrives and is ignored.)

Two things to know before you start:

- **A kit you built is not offered firmware updates over the air.** Your partition table may
  not match ours, and a brick you cannot recover is a bad first day. Update by reflashing.
- **A device token may only subscribe to its own directive topic.** Publishing there is the
  gateway's privilege.

### Talking to it

**Say "Jarvis."** That is the wake word, and nothing is sent anywhere until the kit hears
it — the microphone feeds ESP-SR WakeNet on the device, and only a detection opens a voice
session. The session closes again after `BUDDY_VOICE_IDLE_S` of quiet.

It is a **stock Espressif WakeNet9 model** (`CONFIG_SR_WN_WN9_JARVIS_TTS`), one of several
that ship with ESP-SR, not something trained here. To use a different one, pick it under
*ESP Speech Recognition* in `idf.py menuconfig` and reflash — the model is packed into the
`model` partition at build time, so changing it changes `srmodels.bin`.

Two known rough edges, both on the detector rather than the pipeline:

- **It may wake itself** if the Buddy's own persona is also called Jarvis and the detector
  is in its aggressive mode (`CONFIG_BUDDY_WAKE_THRESHOLD_MODE=1`).
- **Half-duplex while speaking.** The microphone's uplink is muted while the Buddy talks,
  so you cannot interrupt it just by speaking. On the P4, which has echo cancellation,
  saying "Jarvis" over a reply still cuts it off; the S3 has none, so there it waits for
  the reply to end. See [Open questions](#open-questions).

### Being left alone: the clock

After **three minutes of quiet** (`CONFIG_BUDDY_CLOCK_IDLE_S`) the face gives way to the
time and the date, and the kit **stays awake and keeps listening**. "Jarvis" still works,
and the first touch puts the face back. Touching the screen counts as somebody being
there, so a child swiping through the photos never sees it.

The time comes from SNTP once the kit has an address, in the zone named by
`CONFIG_BUDDY_TZ` — `ICT-7` is UTC+7, and note that POSIX inverts the sign. A kit that has
never been online keeps its face instead, because a clock reading 00:00 is worse than no
clock.

### Deep sleep, and why idle no longer reaches it

`CONFIG_BUDDY_SLEEP_IDLE_S` now defaults to **0**. Idle used to blank the panel and enter
deep sleep, which made a kit left on a shelf look broken, and on the P4 it could only be
undone with the RESET button — BOOT there is GPIO 35, not an LP GPIO, so it cannot wake
the chip. Being left alone meant needing an adult.

What remains is the explicit `device.sleep` directive, for someone who asked. **While it
is asleep, "Jarvis" does nothing** — deep sleep has no microphone, so the button is the
only way back, and on a board whose button cannot wake it there is no way back at all.
Read `CONFIG_BUDDY_SLEEP_WAKE_GPIO` before turning the idle timer back on.

Waking is a **cold boot**: RAM, Wi-Fi, TLS and the MQTT session are all gone. The screen
is drawn before the radio starts, so the face is back long before the network is.

Two details that are deliberate rather than incidental:

- **The wake pin must be an RTC GPIO (0–21).** Waking from deep sleep uses `ext0`, which
  the S3 supports on those pins only — `SOC_RTCIO_PIN_COUNT` is 22. GPIO 47 cannot do it at
  any speed, which is why the button is BOOT. A production enclosure's button has the same
  constraint. `CONFIG_BUDDY_SLEEP_WAKE_GPIO` is range-limited to 0–21 so this cannot be got
  wrong quietly, and `sleep_now()` refuses to sleep rather than sleep with no way back.
- **`device.sleeping` is published and waited for** before the radio goes down. An event
  that was queued but never sent is how a kit that works perfectly disappears from the
  website — the same lesson this firmware already learned on the BLE provisioning
  notification.

**Not yet confirmed on hardware:** GPIO 0 is a strapping pin, and held low at reset it
selects download mode. A press-and-release to wake should be clear of the latch, but
nobody has checked. It is a minute to test: let the kit sleep, press BOOT, and see whether
it boots or lands in the bootloader.

**There is no fallback pin any more.** On the GC9A01 board GPIO 21 was the spare to move
to if BOOT misbehaved. With the ST7701S every RTC GPIO is spoken for — 0–18 by the panel
and the audio, 19/20 by native USB, 21 by PCLK — and BOOT works at all only because
`sleep_now()` blanks the panel and takes GPIO 0 back from the LCD peripheral (it is DB17)
before arming `ext0`. If BOOT turns out not to work as a wake pin, the fix is a different
panel pin-out that frees an RTC GPIO, not a different number in `menuconfig`.

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
is), sizes it to 480×480 and re-encodes it as a baseline JPEG, which is the only kind the
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

## Building

Build with **ESP-IDF v5.5.3**. There is no Dockerfile: Espressif's own image is the build
environment, and a native install of the same version works identically. Older IDF does
not build this tree — the P4's panel driver needs 5.4 or later.

Dependencies are **vendored in `components/`**, so the component manager is switched off
and nothing is fetched from the registry. Clone LVGL first (below), then, from the repo
root:

    # ESP32-S3-WROOM-1-N16R8
    docker run --rm -e IDF_COMPONENT_MANAGER=0 -v "$PWD":/fw -w /fw espressif/idf:v5.5.3 \
        idf.py -B build-s3 -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-s3/sdkconfig \
               -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.n16r8" build

    # ESP32-P4
    docker run --rm -e IDF_COMPONENT_MANAGER=0 -v "$PWD":/fw -w /fw espressif/idf:v5.5.3 \
        idf.py -B build-p4 -DIDF_TARGET=esp32p4 -DSDKCONFIG=build-p4/sdkconfig build

Keep the `-B` and `-DSDKCONFIG` pair. Without them both boards share `build/` and the root
`sdkconfig`, and an `sdkconfig` left over from one board, or from an older checkout,
quietly wins over the defaults files — a build for the panel you do not have.

The container cannot see a USB port, so flash from the host ([Flash it](#flash-it)).

`components/lvgl` is not committed. Clone it once, before the first build:

    git clone --depth 1 --branch release/v9.2 https://github.com/lvgl/lvgl.git components/lvgl

A gallery build (`CONFIG_BUDDY_SCREEN_GALLERY`) also needs `gallery/gallery.bin` baked
first ([The gallery](#the-gallery)); the face build carries its art in `face_assets.c` and
needs no photos.

The panel driver glue is written here rather than pulled from `esp_lvgl_port`: a display,
one partial draw buffer in internal RAM, a flush that copies into the PSRAM frame buffer
(`main/st7701.c`), and a tick from `esp_timer`. One fewer dependency.

## Setting a kit up

A kit that was never set up boots into **BLE setup** and advertises as its serial,
`ESP32-<factory MAC>`, with the manufacturer data (`0x4F4C` + the serial's last five
characters) that stem-fe's *Set up with Bluetooth* filters on. The GATT contract is
the other STEM kits', carried over into `components/buddy_ble` (which is in this repo and
is the complete implementation), so the website drives it unchanged:

1. `{"params":{"scan_wifi":1}}` → the kit scans and answers `wifi_list`.
2. `{"params":{ssid, password, user_jwt, mqtt_jwt, env, gateway}}` → the kit joins
   Wi-Fi (30 s budget) and answers `wifi_connection` — `successful`, or `failed`
   with `10002` wrong password / `10004` network not found / `10006` other.
3. On success it saves everything to NVS (`main/prov.c`), **shuts BLE down**, joins
   the broker and publishes `device.activate` (`device_type: buddyos_display_kit`)
   until the gateway answers `device.activate_status`. The website learns the
   outcome from the gateway, not from BLE (ADR 0006).

`gateway` is new with this kit: the stem-gateway host its voice socket dials
(`wss://<gateway>/v1/stem/voice/stream`), sent by stem-fe from
`VITE_STEM_DEVICE_GATEWAY_HOST`. Setup credentials always win over the bench ones in
`sdkconfig`, which are used only when nothing was provisioned. **Hold BOOT for 5 s** to
erase the setup and come back up advertising, even on a bench build. BOOT is a panel data
line on this board, so it is read only at boot: let the kit sleep, then press BOOT and
**keep holding** — it wakes, sees the button still down, and resets after 5 s. (Holding
BOOT through a cold reset lands in the ROM bootloader instead.)

### Flashing the N16R8 board

It uses `partitions-16mb.csv`: two 4.5 MB app slots, then `faces` (the character
catalogue) and `model` (the wake word). **After the partition table changes, flash all the
images** — the app alone leaves the old table in place and the kit looks for its model and
characters where they no longer are. The command is in [Flash it](#flash-it).

## The Waveshare ESP32-P4 board

The same firmware also builds for the **Waveshare ESP32-P4-WIFI6-Touch-LCD-4C**: an
ESP32-P4 (rev 3, 32 MB PSRAM, 32 MB flash), a 4" 720×720 JD9365 MIPI-DSI panel, GT911
touch, an ES8311 DAC with its amplifier, an ES7210 four-channel ADC with two microphones,
and an **ESP32-C6** for Wi-Fi 6 and BLE. Nothing is wired by hand — every pin is the
board's (`main/board_pins.h`, from Waveshare's BSP).

What differs from the S3 kit:

| | ESP32-S3 bench kit | ESP32-P4 board |
| --- | --- | --- |
| Panel | ST7701S RGB, `main/st7701.c` | JD9365 MIPI-DSI, `main/lcd_p4.c` |
| Photos | `gallery/gallery.bin` (480) | `gallery/gallery-720.bin` — bake with `-Side 720` |
| Audio | INMP441 + MAX98357A, `main/audio.c` | ES7210 + ES8311 through `esp_codec_dev`, `main/audio_p4.c` |
| Wake word | WakeNet on the raw mic, half-duplex | ESP-SR AFE with **echo cancellation** (`BUDDY_AEC`) |
| Wi-Fi, BLE | on-chip | the C6, over SDIO, via `esp_hosted` + `esp_wifi_remote` |
| Wake from sleep | BOOT | **RESET** (BOOT is GPIO 35, not an LP GPIO) |

Build — IDF 5.5, with `ESP_IDF_VERSION=5.5` in the environment (esp_wifi_remote's
Kconfig picks its files by that variable; without it the Wi-Fi remote menu silently
vanishes and esp_hosted falls back to SPI). IDF layers `sdkconfig.defaults.esp32p4`
over `sdkconfig.defaults` by itself:

    idf.py -B build-p4 -DIDF_TARGET=esp32p4 -DSDKCONFIG=build-p4/sdkconfig build

The C6 must run esp_hosted's slave firmware; Waveshare ships it so.

This board uses `partitions-32mb.csv` and 32 MB of flash, because the face is baked for
its own panel and that costs what the pixels cost — see below. Everything in that table
stays under 16 MB: a first cut pushed `model` past that line, esptool wrote and verified
it there, and the firmware then read the partition back as garbage through the flash
cache and boot-looped inside ESP-SR's model loader. Proving out >16 MB mapping is its
own job; until someone does it, the top half of the chip is unused.

### Re-baking the face for a panel

Both bakes are committed, so the firmware builds without the baker and without the
website repo: `main/generated/face_assets_720.c` for this board's JD9365 and
`face_assets_480.c` for the S3's ST7701S. `main/CMakeLists.txt` compiles exactly one,
chosen on `IDF_TARGET` — they define the same symbols. The art is vector, so a new size
re-rasterises rather than upscales. The cost is area, in ARGB8888: about 1.8 MB at 480
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
memory-maps them, so they cost no RAM; `tools/bake_catalogue.ts` generates that
image. Neither the partition's contents nor the baker are in the repo — the baker is
untracked for the same reason `bake_face.ts` is, and the partition is written by
hand — so a kit shows the one character baked into the firmware until someone bakes
and flashes the rest:

    cd ../buddyos-stem-fe && npx esbuild ../buddyos-stem-display-firmware/tools/bake_catalogue.ts \
        --bundle --platform=node --format=cjs --outfile=/tmp/cat.cjs \
        --loader:.webp=dataurl --loader:.png=dataurl \
    && node /tmp/cat.cjs face_catalogue.bin

    esptool --chip esp32p4 -p PORT write-flash 0xd12000 face_catalogue.bin   # P4
    esptool --chip esp32s3 -p PORT write-flash 0x912000 face_catalogue.bin   # S3 N16R8

The offset is the `faces` partition's; `partitions-32mb.csv` (P4) and
`partitions-16mb.csv` (S3) are the authority, and it moves if the table changes. The kit says what it found at boot — `3 characters
mapped from flash, 1506 KB, no RAM` — and names them.

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

### Setting up by camera

The setup screen shows the serial as a **QR** as well as text (`CONFIG_LV_USE_QRCODE`;
LVGL vendors the widget, so there is no new component). It carries the serial alone, not
a URL: stem-fe's `ScanCode` step hands whatever it decodes straight through as the
serial, so a URL would fail the BLE name filter downstream — an empty chooser with
nothing to debug. The printed serial stays above it, because a camera that will not focus
is not a reason to be unable to set a kit up.

### Echo cancellation and the microphone slots

The ES7210 hands the P4 four TDM slots. `BUDDY_AFE_INPUT_FORMAT` says what each one is,
in ESP-SR's letters — `M` microphone, `R` playback reference, `N` unused — and defaults
to `MRMN`: mic, reference, mic, nothing, as Espressif's P4 boards wire it. If the board
has **no** hardware reference (no `R` in the string), the firmware turns the first `N`
into a software reference: every sample written to the speaker is also queued, aligned
by the DMA depth, and fed to the AFE in that slot. A hardware loopback is better — it
hears the amplifier, not the intention — so prove which one the board has before
trusting either.

`BUDDY_AUDIO_SELFTEST` is how: it prints each slot's level in silence, then plays a
440 Hz tone and prints them again. The slot that rises most is the reference; it warns
if that is not the one the format string calls `R`. Then it shows a live four-slot meter
for ten seconds — talk at each microphone to find which slots they are.

With AEC on, WakeNet listens to the AFE's cleaned output, so "Jarvis" works while the
Buddy is talking, and the microphone stays open during replies. `BUDDY_AEC_KEEP_HALF_DUPLEX`
puts the old behaviour back — mic muted while the Buddy speaks — if the cancellation
turns out not to be enough for the gateway's speech detector.

An open microphone is also what lets the child interrupt **without** the wake word: the
cloud hears them over the reply and stops on its own, then sends a `flush_audio` status.
The kit empties its playback queue on that, because the cloud cannot — half a second of
the abandoned sentence is already here, queued and in the codec's DMA, and playing it
out would sound exactly like not having been heard. Saying "Jarvis" still works and is
still faster, since it needs no round trip. Whether the cloud interrupts itself at all
depends on the persona: `profile_settings.interruption` must not be `none`.

> **The kit needs `BUDDY_UPLINK_GAIN_DB` for any of this to work**, and until
> 2026-10-22 it did not have it. Without the gain the kit was audible to omni's
> recogniser and invisible to its interrupter, which looked like "barge-in is broken"
> and was really "the audio is 40 dB too quiet".
>
> Measured against staging omni's own VAD telemetry, with a working Heymates device on
> the same cluster for comparison:
>
> | | loudness | Silero score | speech episodes |
> | --- | --- | --- | --- |
> | kit, no uplink gain | -62.9 dBFS | 0.03 | none in 114 s |
> | kit, +30 dB | -29.3 dBFS | 0.60-0.83 | start/end, 2.8 s |
> | `HHMST26330000006` | -22.5 dBFS | 0.91-0.97 | normal |
>
> With the gain, barge-in works on the **fast VAD path**, with `vad_processor` loaded
> and left on its defaults — `speaking_started` 30951, `flush_audio` 45219,
> `interrupted` 45268. Talking over the Buddy stops it, as it does in a browser.
>
> Two theories this killed, both of which had been written down as fact. Silero was
> **not** rejecting the kit for being 16 kHz upsampled to 48 — it simply never heard
> speech at -63 dBFS. And `loudness_at_onset` was never the blocker: the editor's
> "Very sensitive" removes that gate and changes nothing, because a 0.03 score fails
> the probability threshold long before any loudness gate is consulted.

### How hard to suppress the echo

`CONFIG_BUDDY_AEC_NLP_LEVEL` — 0 normal, 1 aggressive, 2 very aggressive, **default 2**.
It is a knob rather than a constant because the failures either side of it are both real
on this board, and which one bites depends on the enclosure, the volume and the room.

Too low and the kit hears its own speaker: omni's speech detector takes the Buddy's
voice for the child and the Buddy interrupts itself mid-sentence. Caught in a log at
NORMAL — `speaking_started` 45410, `interrupted` 47819, two and a half seconds into its
own reply.

Measured on the board, echo left in the microphone while the Buddy talks, against a
reference slot at about 15000/32768:

| `BUDDY_AEC_NLP_LEVEL` | residue in the mic |
| --- | --- |
| 1 aggressive | `M:83/833` |
| 2 very aggressive | `M:37/536` |

Turning it to the top roughly halves what leaks through. The cost it is supposed to
carry — NLP suppresses whoever is quieter, and the child is quieter — did not show up:
the wake word was heard 3 of 4 times at aggressive and 5 of 6 at very aggressive. Both
samples are far too small to separate, which is the honest reading; what they do rule
out is the collapse that made this NORMAL in the first place.

**The real fix is level, not suppression.** This board's microphone delivers speech at
about -47 dBFS where `voice.c` reckons arm's length to be -25 — roughly 22 dB quiet.
That is what puts WakeNet near its threshold at *any* NLP setting, what puts the child
below omni's -30 dB VAD gate, and so what forces the trade at all. Note that raising
`CONFIG_BUDDY_MIC_GAIN_DB` is **not** that fix: 30 -> 37 lifted the *reference* slot
about twentyfold rather than the microphones, wrecking the cancellation it was meant to
help. It wants the ES7210's per-channel gain instead.

**What bring-up found** (2026-10-21, one board, P4 silicon v3.2):

- **Ran on the board as written:**
  - the JD9365 panel (ID `93 65 04`);
  - the GT911 at `0x5D`;
  - the C6's hosted firmware over SDIO;
  - BLE setup through the C6;
  - Wi-Fi and MQTT;
  - the AFE pipeline (`AEC → BSS → WakeNet`, slots `MRMN`).
- **The chip revision decides which ESP-SR links.** v3 changed the encoding of the P4's
  vector (PIE) instructions:
  - ESP-SR 2.1.2's P4 libraries were built for older chips. WakeNet's model load faulted
    (`MCAUSE 0x1f` in `dl_esp32p4_memcpy`).
  - The voice session starts once Wi-Fi is up, so the kit rebooted every time it got
    online. From outside, that looked like setup failing.
  - The fix was to move to ESP-SR 2.4.7 (`components/esp-sr/VENDORED.md`).
- **The board has two USB-C ports:**
  - one is the P4's own USB-Serial/JTAG;
  - the other is a CH343 UART.
  - Either one flashes and shows the log, and Windows gives them different COM numbers.
- **"Jarvis" is heard over the Buddy's own voice** and cuts a reply off in ~200 ms. Not yet
  measured: how much of that survives a real, noisy room. With the AFE running, the
  internal heap's largest free block is about 30 KB, so watch it when a session opens.

## Status

**It runs on the bench board**: an ESP32-S3-WROOM-1-N16R8 driving the ST7701S 480×480
round panel and its touch layer, with the gallery up, Wi-Fi and the product broker
connected, and the wake word listening. Measured on it, 2026-10-20, on a kit already set
up (so BLE is down), with `CONFIG_BUDDY_AUDIO` and `CONFIG_BUDDY_VOICE` on:

| Stage | Internal free | min-ever | largest block | PSRAM free | LVGL used |
| --- | --- | --- | --- | --- | --- |
| boot | 259,295 | 227,264 | 163,840 | 6,521,364 | — |
| gallery up (two photos decoded) | 215,287 | 179,156 | 139,264 | 5,125,504 | 8,108 of 14,644 |
| wifi connected | 112,723 | 76,548 | 34,816 | 5,058,520 | 8,108 |
| mqtt connected (TLS + JWT, WakeNet loaded) | 66,487 | 39,672 | 31,744 | 4,519,932 | 8,108 |
| running, 40 s later | 66,587 | 39,672 | 31,744 | 4,519,764 | 8,108, peak 8,660 |

**Voice did not fit at first.** The same build with the first settings reached `mqtt
connected` with 14,907 B internal free, a largest block of 6,144 and a min-ever of 1,732:
the wake word fired, the 8 KB WebSocket task stack could not be allocated, and the gallery
could not get 4 KB to decode a photo. Against the voice-off figures below, voice costs
~66 KB of internal SRAM — ~21 KB of ESP-SR statics, ~23 KB of I2S DMA (six 20 ms
buffers each way, 32-bit slots), the rest WakeNet and the voice tasks. What gave it back:

- **LVGL pool 32 → 16 KB.** It peaks at 9.2 KB here.
- **Wi-Fi static RX buffers 16 → 8**, BA window with them. Internal and DMA-capable,
  ~1.6 KB each, held from `esp_wifi_init()`; this kit streams a few KB/s of voice.
- **`EXT_RAM_BSS_ATTR` on the CPU-only statics** — the gallery's photo table, the Wi-Fi
  scan lists, the voice PCM scratch — with `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`.
- **The gallery's 4 KB decode work area falls back to PSRAM** when internal says no: a
  slower decode beats a photo that never appears.

Wi-Fi itself is the largest single consumer, ~90 KB from `esp_wifi_init()` to connected;
the panel's draw buffer and bounce buffers are another 44 KB, taken before the radio.

For comparison, with `CONFIG_BUDDY_AUDIO` off and the first settings (LVGL 32 KB, 16 RX
buffers), as the bench build had it:

| Stage | Internal free | min-ever | PSRAM free | LVGL used |
| --- | --- | --- | --- | --- |
| boot | 237,807 | 205,776 | 6,949,976 | — |
| gallery up (two photos decoded) | 193,799 | 157,668 | 5,554,104 | 8,108 of 30,896 |
| wifi connected | 101,791 | 69,496 | 5,487,992 | 8,108 |
| mqtt connected (TLS + JWT) | 80,763 | 42,416 | 5,455,224 | 8,108 |
| running, 50 s later | 80,863 | 42,416 | 5,455,056 | 8,108, peak 8,660 |

The app image is **1.82 MB** with voice in a 3 MB slot (39% free); 1.50 MB without.

### What only hardware found

On the first, GC9A01 board — worth keeping, because each would have cost the next person
the same bench day:

- **A reset loop on the very first boot.** `mem_report("boot")` asks LVGL for a heap
  monitor *before* `lv_init()`, and LVGL 9.2 walks an empty pool list, leaves `total_size`
  at 0 and divides by it.
- **Every colour wrong on an SPI panel.** LVGL stores RGB565 low byte first; the GC9A01
  clocked the high byte first. The ST7701S is fed 16 data lines at once, so this build
  does not swap — see `flush_cb` in `main/main.c`.
- **240 MHz and `-O2` were never turned on.** 2.3x for no code and no memory; they are in
  `sdkconfig.defaults` now.

On this board: the **main-task stack overflow** described under [The
gallery](#the-gallery), and **PSRAM cannot run at 120 MHz** — the board's flash has no
high-performance mode, so it stays at 80 MHz.

### Audio, proven on hardware 2026-09-28

On the N16R8 bench board, with every pin in `main/board_pins.h`: the MAX98357A plays the
self-test tone, and the INMP441 hears the room — quiet sits around -40 dBFS, speech and
claps push the peak to 24,184 of 32,768 (74% of full scale), about 30 dB above the floor,
and it falls back when the room does.

`CONFIG_BUDDY_AUDIO` brings both up; `CONFIG_BUDDY_AUDIO_SELFTEST` adds a tone and a
ten-second live meter of both halves of the I2S frame. Both default off.

Four things that each cost a bench round, written down so they cost nothing next time:

- **The INMP441 outputs zeros for its first 2^18 clock cycles** (~85 ms here), then
  settles with a transient large enough to read as near full scale. Listen too early and
  a silent room reads -4 dBFS. The self-test discards the wake-up.
- **Its DC offset is large and wanders** in the first seconds (+646 on one boot, -7,653 on
  another) while its internal high-pass settles. Remove it before measuring anything, and
  put a high-pass in front of the encoder in the streaming path.
- **A flat level is not proof of a dead microphone** — it is also what a quiet room
  looks like. The meter only means something while someone is talking at it.
- **Power the amplifier from 5 V, not the board's 3V3.** On the first board the 5 V rail
  read 4 V under load, the 3V3 regulator ran out of headroom, and the USB-serial bridge
  was the first thing to fail: dropped bytes, re-enumeration, and a board that looked
  bricked when it was only starved.

### It reaches the product broker

An EMQX broker over `mqtts://`, with the device JWT as the password, accepted this
firmware on 2026-09-27, and the token's ACL allowed both topics it needs:

    net: TLS: verifying against the embedded Amazon Root CA 1
    net: connecting as buddyos-stem-<env>-ESP32-<serial> (token 1267 B)
    net: mqtt connected
    net: subscribed: <env>/stem/device/ESP32-<serial>/directive
    net: published  <env>/stem/device/ESP32-<serial>/event

Connecting only proves the token was accepted; the subscribe and the publish prove the ACL
*inside* it allows what the device actually needs, which is a separate question and the one
that bites later. Both are done on connect, so a bad token is visible in the log rather than
at the first directive.

The client follows the MQTT client our other STEM firmware already uses rather than
inventing a second way to reach the same broker. Three details are carried over because
each is a silent failure otherwise:

- **`buffer.size = 4096`.** The device JWT is ~1.3 KB, so the default 1 KB buffer cannot
  hold the CONNECT this client sends. The symptom is a broker that appears to hang up.
- **One root, not the bundle.** The broker sits behind an AWS certificate, so
  `certs/AmazonRootCA1.pem` is embedded: **6 KB of flash against the bundle's 66 KB**, and it
  verifies rather than skipping. `CONFIG_BUDDY_MQTT_AWS_ROOT=n` switches back to the bundle
  for a broker with some other publicly-trusted certificate.
- **Topic namespace must match the token's `environment` claim.** Topics are
  `{env}/stem/device/{username}/…` and the ACL names them literally; a mismatch is denied,
  not misrouted.

Credentials live in `sdkconfig`, which is gitignored. Nothing in this repo carries a token.

### Can the voice path fit? The memory part, measured

Three runs, each against the product broker, answering the question the planning docs have
been holding open:

| | internal free | min-ever | largest internal block | PSRAM free |
| --- | --- | --- | --- | --- |
| EMQX + JWT, default config | 129,895 | 126,459 | **32,768** | 1,998,152 |
| …with `MBEDTLS_EXTERNAL_MEM_ALLOC=y` | 161,495 | 156,199 | **61,440** | 1,998,152 |
| …plus a second, verified TLS session | 152,991 | 152,763 | **59,392** | 1,966,916 (min 1,952,616) |

**One Kconfig symbol nearly doubles the contiguous internal ceiling.** mbedTLS allocating
from PSRAM moves the session's ~32 KB out of internal SRAM, which has nothing else to offer
it, into 2 MB that has nothing else to do. Total free barely moves; contiguity is what this
buys, and contiguity is what a DMA buffer or a second TLS context actually needs. It is now
in `sdkconfig.defaults`.

**A second concurrent TLS context costs ~2.5 KB internal and ~28 KB of PSRAM** (≈42 KB at
the handshake peak, all returned on close). That is the closest thing to esp-webrtc's
DTLS-SRTP that can be measured without esp-webrtc, and it is not close to a problem.
`CONFIG_BUDDY_TLS_PROBE` runs it; it is off by default because a shipped device has no
business dialling an outside host.

This is a proxy, and worth naming as one: DTLS is not TLS, SRTP is extra, and esp-webrtc
also brings an ICE agent, a jitter buffer and a media pipeline. What it settles is the
specific fear this repo's documents were written around — *a second TLS context on top of
the broker's* — and that fear was misplaced.

### What TLS costs, which is the number to carry forward

*Measured on the GC9A01 / N8R2 board, before this build; "face up" was the screen's own
cost then. The lesson — contiguity, not total free — still holds.*

A connected plaintext broker leaves 171,711 B free with a 69,632 B largest block. The same
broker over TLS leaves **137,863 B** free with a **36,864 B** largest block, and the
low-water mark through the handshake is **130,175 B** — so a single MQTT TLS session costs
about **36 KB at its worst moment** and, more to the point, **halves the largest contiguous
block a second time**:

    largest contiguous internal block
    boot            188,416
    face up         163,840
    radio up         81,920
    mqtt (plain)     69,632
    mqtt (TLS)       36,864
    EMQX + JWT       32,768   <- what the product actually runs

Total free is not the thing that will break. Contiguity is: this product line has twice been
bitten by it, and once it lied about the cause — *a starved handshake reported itself as
`NOT_TRUSTED`*, sending the reader after certificates. **esp-webrtc's DTLS-SRTP would be a
second TLS context on top of this one**, and it would be asking for large blocks from what
is left of a 36 KB ceiling. That is the question the voice work has to answer, and it now
has a floor to answer it from.

TLS also costs **65,813 B of flash** (1,499,397 B image against 1,433,584 B) and 424 B of
static D/IRAM, almost all of it the certificate bundle.

Measured against `test.mosquitto.org:8886` — the listener with a publicly-trusted
certificate; 8883 is signed by mosquitto's own CA, which the bundled roots do not contain.
That broker drops an idle connection after about a minute (`transport_read(): EOF`), which
is the broker's policy and not a fault here.

## Taking the measurement

    idf.py menuconfig          # BuddyOS display firmware -> SSID, password, broker
    idf.py -p /dev/ttyUSB0 flash monitor

`sdkconfig` is gitignored, so credentials set there stay on the bench. On macOS the
container cannot see the serial port: build in Docker, then flash from the host
(`python3 -m venv venv && venv/bin/pip install esptool`) with the command `idf.py build`
prints. A `mqtts://` broker URI switches on certificate verification against IDF's bundled
public roots; nothing else needs changing.

Every milestone logs one `mem` line — `boot`, `gallery up`, `wifi init`, `wifi started`,
`wifi connected`, `mqtt started`, `mqtt connected`, then `running` periodically and
`gallery pN` after every swipe:

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

## Open questions

- **Current `master` has not run on the S3 board.** The S3 N16R8 last ran on 2026-10-20,
  with the gallery build and voice on ([Status](#status)). Everything since — the 480×480
  face, the two-board merge, the idle clock, the volume control — has been run on the P4
  and only compiled for the S3. Reports from an S3 are welcome.
- **Voice sessions drop on a congested Wi-Fi network.** Seen on the P4: a conversation
  closes a second or two after it opens, and the status cycles *connecting → listening*.
  Every WebSocket send in `main/voice.c` waits at most 200 ms, and `esp_websocket_client`
  treats one write that takes longer as a dead connection. Raising the limit is not a
  one-line fix: the 200 ms keeps the microphone task inside its 20 ms budget.
- **Interrupting the Buddy without the wake word.** The uplink is muted while the Buddy
  speaks, because every partial setting tried either let the Buddy interrupt itself or
  stopped a child interrupting it (`sdkconfig.defaults.esp32p4` has the numbers). Real
  voice barge-in needs the voice server to stop treating audio sent while it is speaking
  as an interruption; it cannot be fixed in this firmware alone.
- **BOOT as a wake pin on the S3** is not confirmed on hardware. GPIO 0 is a strapping pin,
  so a press might land in the bootloader instead of waking the kit. It matters only for
  the explicit `device.sleep` command now that idle shows a clock — see [Deep
  sleep](#deep-sleep-and-why-idle-no-longer-reaches-it).

## Licence and support

This firmware is **Apache-2.0** — see [`LICENSE`](LICENSE). Vendored dependencies keep their
own licences and are listed in [`LICENSE-THIRD-PARTY.md`](LICENSE-THIRD-PARTY.md) — mostly
Apache-2.0 and MIT, but `esp-sr` (the "Jarvis" wake word) and `esp_audio_codec` are under
Espressif's licences, which allow use only on Espressif chips.

The **cloud is not open source.** The gateway, the voice pipeline, the broker and the identity
service are a hosted product, and a kit uses them through a normal account. That is what the
licence covers and what it does not.

**What we answer:** this firmware building, the protocols it speaks, and a device that
provisions but will not come online. **What we cannot:** your wiring, your module, your
soldering, and boards other than the one named in [Build one yourself](#build-one-yourself).
Nothing here promises a response time.

Credentials live in `sdkconfig`, which is gitignored. **Nothing in this repo carries a token**
— if you find one, that is a bug worth reporting privately rather than in an issue.
