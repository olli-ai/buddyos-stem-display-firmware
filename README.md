# buddyos-stem-display-firmware

ESP-IDF firmware for the **screen-equipped BuddyOS STEM device**: the kit that shows the
Buddy a child designed on the website, drawn with LVGL, lip-syncing to the voice it speaks
with, and animated from the child's own keyframes.

## Which board, which branch

Two boards carry this device type, and **each has its own branch**. Build the one that
matches the hardware in your hand — they are not one firmware with a flag, and the wrong
branch will not fit.

| Your board | Branch | Screen |
| --- | --- | --- |
| **ESP32-S3-WROOM-1-N16R8** (16 MB flash, 8 MB PSRAM) | **`master`** — you are here | 1.28" round GC9A01, no touch |
| **ESP32-P4** (32 MB flash) e.g. Waveshare ESP32-P4-WIFI6-Touch-LCD-4C | **`esp32p4-touch-screen`** | 4" 720x720 JD9365, touch |

```sh
git clone https://github.com/olli-ai/buddyos-stem-display-firmware.git
cd buddyos-stem-display-firmware
git checkout esp32p4-touch-screen        # only if you have the P4 board
```

**Why not one branch.** The face is vector art rasterised offline, so its cost is area:
the bake this branch carries is 3.0 MB of generated source, and the P4's 720x720 bake is
26 MB, which links to a 5.6 MB image against this board's 3 MB app slot. Merging them
leaves a tree that cannot build for either board — measured, after trying it. Unifying
would mean a per-target asset set, or the baker in the repo so artwork is generated at
build time rather than committed; until then, a branch each.

The P4 branch is also where the newer work lives: the PPA compositor, catalogue
characters, model-chosen expressions, touch reactions and the setup QR.

## Setting your kit up

Once the firmware is flashed, everything else happens at **<https://app.buddystem.com>** —
create an account, add the kit over Bluetooth, design its face, give it a voice and watch
what the cloud costs. A kit you built yourself appears there exactly like one that came in
a box.

**You can build one.** The parts are off-the-shelf modules on a breadboard, this firmware is
Apache-2.0, and a kit you flashed yourself joins the BuddyOS cloud exactly like one that came
in a box — same provisioning, same studio, same face designer. See
**[Build one yourself](#build-one-yourself)**.

> **This README is an engineering record**, not a tutorial. It is long on purpose: every
> measurement here was taken on real hardware, and most of the surprising ones cost a bench
> day. If you only want to get a kit working, read [Build one yourself](#build-one-yourself)
> and [Setting a kit up](#setting-a-kit-up), then stop. The rest is here for when something
> does not behave.

## What this repo is for

1. **Panel bring-up** — driver, LVGL, a frame on the glass.
2. **The face renderer** — composite catalogue sprites into a face from a few hundred bytes
   of the child's choices. Never pixels over the wire; part ids and colours.
3. **The `.buddy` motion interpreter** — walk keyframe tracks, drive `lv_anim`. The format
   is defined by the website's own implementation, and its behaviour is pinned by a set of
   conformance fixtures. **Those fixtures are the contract**: this firmware is correct when
   it reproduces them, including the per-easing LUT checksums. The goldens are vendored
   here as `main/generated/` and `tools/`, so `tools/xcheck.sh` runs with nothing but this
   repo — see [Holding it to the website's answer](#holding-it-to-the-websites-answer).
4. **The catalogue partition** — artwork delivered and updated without a firmware
   release: a presigned URL streamed to flash and CRC-checked before it is committed.
5. **Lip-sync** — the mouth follows the audio the Buddy is speaking, never its text. See
   **Lip-sync** below.

## What this repo is *not*

**It is a face, a voice and a screen — nothing else.** The whole 2 MB app image goes on the
panel driver, the face renderer and its catalogue, the motion interpreter, the wake word
and the voice path, and that is already a tight fit: see [Static
sizes](#static-sizes) for where every kilobyte goes.

The only inference on the device is the wake word (ESP-SR WakeNet, in its own partition)
and the lip-sync classifier, which is a 512-point FFT and a few thresholds rather than a
model. Anything heavier than that does not fit beside what is here, and the measurements
in this README are the argument.

## Build one yourself

Everything here is a common module and a breadboard. No PCB, no reflow, no custom parts.

| Part | What to get | Notes |
| --- | --- | --- |
| MCU | **ESP32-S3-WROOM-1-N16R8** devkit — 16 MB flash, 8 MB PSRAM | **Not the N8R2.** See below. |
| Display | **GC9A01** 1.28" 240×240 round TFT, SPI | The bring-up module has no backlight pin; one that exposes it is better. |
| Microphone | **INMP441** I²S MEMS breakout | The same part the Robot Kit uses. |
| Amplifier | **MAX98357A** I²S breakout | |
| Speaker | 4 Ω, 2–3 W, small | |
| Power | A **5 V supply for the amplifier**, not the board's 3V3 | Not optional — read the warning below. |

**Buy the N16R8.** The older **N8R2** (8 MB flash, 2 MB PSRAM) runs the face fine, but its
2 MB app slots cannot hold the voice path *and* BLE setup together; the build fails at link
time. The 16 MB table (`partitions-16mb.csv`) gives 3 MB slots and the app lands at 2.14 MB.

**Power the amplifier from 5 V.** On the first bench board the 5 V rail read 4 V under load,
the 3V3 regulator ran out of headroom, and the first thing to fail was the USB-serial bridge:
dropped bytes, re-enumeration, and a board that looked bricked when it was only starved. If
your board behaves strangely the moment audio starts, this is why.

### Wiring

`main/board_pins.h` is the source of truth — if this table and that file ever disagree,
the file is right. As wired on the N16R8 bench board:

| Signal | GPIO | Module pin |
| --- | --- | --- |
| **GC9A01** SCLK | 15 | `SCL` |
| MOSI | 16 | `SDA` |
| DC | 17 | `DC` |
| CS | 18 | `CS` |
| RST | 8 | `RST` |
| **MAX98357A** SD | 13 | shutdown — high enables, low mutes |
| GAIN | 12 | see `audio_set_gain()` |
| DIN | 11 | |
| BCLK | 10 | |
| LRC | 9 | |
| **INMP441** SD | 4 | the mic's data *out*, an input to us |
| SCK | 5 | |
| WS | 6 | |
| L/R | 7 | held low = left channel |

Plus 3V3 and GND to the panel and the mic, and **5 V** to the amplifier.

These pins were chosen around what the S3 reserves on this module — octal PSRAM (35–37),
native USB (19/20), flash (26–32) and the strapping pins (0, 3, 45, 46). **GPIO 3 especially:**
it is sampled at reset, and an I²S word clock on it was the first suspect when an earlier
board would not boot cleanly.

The original N8R2 bench board was wired differently (panel SCK 14, MOSI 10, DC 11, CS 12,
RST 13; amp SD 5, DIN 4, BCLK 6, LRC 7; mic SD 17, SCK 16, WS 9, LR 18). Those numbers appear
in older notes and in the header's history comment — they will not work with this build
without editing `board_pins.h`.

### Flash it

Build in Docker (next section), then write the five images the build produces:

    python3 -m venv venv && venv/bin/pip install esptool
    venv/bin/esptool --chip esp32s3 -p /dev/ttyUSB0 \
        --flash_mode dio --flash_freq 80m --flash_size 16MB write-flash \
        0x0      build/bootloader/bootloader.bin \
        0x8000   build/partition_table/partition-table.bin \
        0x10000  build/buddyos_stem_display.bin \
        0x610000 build/ota_data_initial.bin \
        0x792000 build/srmodels/srmodels.bin

**Do not drop the bootloader at `0x0`** on a board that has never run this firmware —
without it there is nothing to start the app, and the board comes up silent in a way that
looks like a hardware fault.

`build/flash_args` is written by the build and is the authoritative list of offsets for
whatever you just built; if it and this README ever disagree, the file is right.

**`face_catalogue.bin` is deliberately not in that list.** It is extra character artwork,
it is *not* produced by `idf.py build` (which is why it is absent from `flash_args`), and
esptool aborts the whole command if any file is missing — so including it would mean
flashing nothing at all. Without it the `faces` partition reads as blank, the mount says so
in the log, and the kit draws the Buddy baked into the firmware. See [The characters live
in their own partition](#the-characters-live-in-their-own-partition) if you want to build
one.

esptool does not come with the Docker build — the container cannot see a USB port, so
flashing happens on the host and the tool is installed there. **`esptool` and `write-flash`,
not `esptool.py` and `write_flash`**: esptool 5 deprecated both older spellings (they warn
now and are slated for removal), and a fresh `pip install esptool` today gives you 5.x.

Your port will not be `/dev/ttyUSB0` on every machine — on macOS it is a
`/dev/cu.usbserial-*` (`ls /dev/cu.*` finds it), on Windows a `COM` number. On Linux, a
*permission denied* means your account is not in the `dialout` group.

`idf.py build` prints the command with the right offsets for whichever partition table you
built against.

With nothing else done, the kit boots, shows the built-in Buddy and cycles through its states.
To put it on the network, carry on to [Setting a kit up](#setting-a-kit-up).

### Bringing it onto BuddyOS

The firmware is free; the cloud it talks to is a paid service, and that is the deal. Register
at the BuddyOS STEM website, then follow the **display-kit setup guide** in its *Documents*
section — it is written for people holding a board, kept in step with this repo, and available
in English and Vietnamese. It covers creating the account, provisioning over Bluetooth,
designing a face and what cloud usage costs.

Two things to know before you start:

- **A kit you built is not offered firmware updates over the air.** Your partition table may
  not match ours, and a brick you cannot recover is a bad first day. Update by reflashing.
- **A device token may only subscribe to its own directive topic.** Publishing there is the
  gateway's privilege, so bench tools like `tools/send_face.py` reach the device by a
  different route (see [Which face, during a conversation](#which-face-during-a-conversation)).

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
- **Half-duplex while speaking.** There is no echo cancellation yet, so the microphone is
  muted while the Buddy talks; you cannot interrupt it mid-sentence by voice.

### Going to sleep, and waking up

After **three minutes of quiet** (`CONFIG_BUDDY_SLEEP_IDLE_S`, 0 disables) the kit shows
its `asleep` face, tells the gateway, blanks the panel and enters **deep sleep**. A
`device.sleep` directive does the same thing on demand. **Press BOOT to wake it.**

**While it is asleep, "Jarvis" does nothing.** Deep sleep has no microphone, so the button
is the only way back. That is the trade for microamps, and it is the thing most likely to
surprise somebody — on a bench, set `CONFIG_BUDDY_SLEEP_IDLE_S=0`.

Waking is a **cold boot**: RAM, Wi-Fi, TLS and the MQTT session are all gone. The design is
restored from NVS before the radio is up, so the face is back long before the network is.

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

**If it misbehaves, use GPIO 21.** The RTC range is 0–21 and most of it is spoken for here
— 4–13 and 15–18 are the mic, amp and panel, 0 and 3 are strapping pins, 19/20 are native
USB — which leaves **1, 2, 14 and 21**. 21 is the pick: 14 sits between the amp and panel
blocks and is the pin you would want back for a peripheral, while 1 and 2 are neighbours of
strapping pin 3, where a jumper one position over fails to boot in a way that looks like
nothing at all. Nothing adjoins 21.

It costs a button and a pull-up to wire, where BOOT is already on the board — so test
GPIO 0 first.

### Try it before you buy anything

If you have not bought anything yet, the [simulator](#simulator) builds the same face
renderer, motion interpreter and lip-sync classifier for your own machine and serves them in
a browser. It is the cheapest way to see what you would be building.

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
`sdkconfig`, which are used only when nothing was provisioned. **Hold BOOT for 5 s**
to erase the setup and come back up advertising, even on a bench build.

### Flashing the N16R8 board

It uses `partitions-16mb.csv` (3 MB app slots; BLE did not fit beside voice in 2 MB), so
faces and the wake-word model moved to new offsets. **After changing the partition table,
flash all five images** — app alone leaves the old table in place and the kit looks for its
catalogue where it no longer is. The command is in [Flash it](#flash-it).

## Watching it without hardware

`sim/` builds the same `face_view.c` / `face_motion.c` / `face_assets.c` for the host and
renders them into a memory buffer. See **Simulator** below.

## Status

**It runs on a panel.** First hardware bring-up 2026-09-27: an ESP32-S3-WROOM-1-N8R2
(8 MB flash, 2 MB embedded PSRAM, chip rev v0.2) driving a GC9A01 240×240 round module over
SPI at 40 MHz. The Buddy appears, in the child's colours, and cycles idle → listening →
thinking → speaking with the mouth following the embedded speech clip.

That board was wired SCK 14, MOSI 10, DC 11, CS 12, RST 13, with no backlight line. **The
current bench board is an N16R8, rewired 2026-09-28, and its pins are different** — see
[Wiring](#wiring), and `main/board_pins.h`, which is the only place that is kept correct.

### What only hardware found

Three things, none of which the simulator could have caught — worth stating, because the
simulator is otherwise so good that it is tempting to trust it further than it goes:

- **A reset loop on the very first boot.** `mem_report("boot")` asks LVGL for a heap
  monitor *before* `lv_init()`, and LVGL 9.2 walks an empty pool list, leaves `total_size`
  at 0 and divides by it. The simulator only ever reports after init, so it never asked the
  question at a moment when the answer was fatal.
- **Every colour wrong.** LVGL stores RGB565 low byte first; the panel clocks the high byte
  first. The face was pixel-perfect in shape and cyan-green with purple contour bands. The
  simulator renders into host memory and displays from host memory, so the two endiannesses
  cancel and it has always looked right. Confirmed *before* touching the board by applying
  the same swap to a simulator frame and matching it against the photograph.
- **The lip-sync classifier cost 134% of a core.** See below.

### Lip-sync on silicon

| | µs of CPU per frame | per 16 ms of audio |
| --- | --- | --- |
| First measurement | 21,433 | **134%** of a core |
| After the ring buffer | 1,777–1,865 | ~11% of a core |
| At 240 MHz with `-O2` | 776–815 | **~4.9%** of a core |

The first version slid its analysis window with a 511-sample `memmove` for **every arriving
sample** — 16 MB/s of copying to advance an audio pointer. A ring buffer walked once per hop
does the same arithmetic in the same order, so the conformance golden did not move: still
149 of 149 frames.

The first two figures were at **160 MHz with `-Og`** — the part's own frequency and the
compiler's optimiser were simply never turned on. Turning both on is 2.3x for no code and
no memory; internal free went *up*, because `-O2` lays the image out differently. They are
in `sdkconfig.defaults` now.

**27x separates the first measurement from the current one**, and only the first factor of
twelve was a bug. The rest was headroom nobody had claimed.

### Static sizes

As of 2026-09-26, ESP32-S3 with Wi-Fi and MQTT linked in, PSRAM enabled:

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

### Runtime, from the board

The figure the planning docs kept asking for, measured 2026-09-27:

| Stage | Internal free | min-ever | largest block | PSRAM free |
| --- | --- | --- | --- | --- |
| boot | 283,919 | 283,919 | 188,416 | 2,094,528 |
| face up | 259,167 | 259,167 | 163,840 | 2,094,244 |
| wifi init | 176,563 | 176,563 | 81,920 | 2,033,072 |
| wifi started | 176,327 | 174,523 | 81,920 | 2,031,500 |
| wifi connected | 175,939 | 174,523 | 81,920 | 2,030,596 |
| mqtt started | 166,667 | 166,667 | 69,632 | 2,029,964 |
| mqtt connected | 171,711 | 166,135 | 69,632 | 2,029,764 |

…the same run over **TLS** to a public test broker, and then to **the product's own EMQX**
with a device JWT:

| Stage | Internal free | min-ever | largest block |
| --- | --- | --- | --- |
| mqtt connected, TLS (mosquitto) | 137,863 | 130,175 | 36,864 |
| **mqtt connected, TLS + JWT (EMQX)** | **129,895** | **126,459** | **32,768** |

The real broker costs ~8 KB more than the test one — a 4 KB client buffer for the token, the
token itself, and a longer client id — and that is the configuration the product will ship,
so **32,768 B is the largest contiguous internal block a feature can actually ask for.**

LVGL's own heap: **6,744 B used of 31,112, peak 6,744, 1% fragmented** with the face up —
against the 13,320 B the simulator predicted, so the 32 KB pool has room to spare.

**The whole face costs 24,752 B of internal SRAM at runtime**, almost all of it the two DMA
draw buffers. Bringing the radio up costs another ~82,700 B and, more importantly, **halves
the largest contiguous block** from 163,840 to 81,920 B — which is the number to watch,
because it is contiguity rather than total free that kills a DMA allocation.

### A face designed on the website, on the device

The flow the whole stack was built toward, working on 2026-09-27:

    studio saves  ->  gateway stores and publishes  ->  kit redraws
                      {env}/stem/device/{serial}/directive

**616 bytes** crossed the wire for a complete redesign — **all seven states** the website
authors, the eyes, mouth and brows of each, and how the Buddy talks. Never pixels. The
artwork is already here, baked from the same catalogue the website draws with, which is the
whole premise of the faces spec and what makes a redesign feel instant rather than feel
like a download.

The device carries all seven — `idle`, `listening`, `thinking`, `speaking`, `busy`,
`error`, `asleep` — because a conversation passes through more than four, and a state the
device does not carry is one a child can design and never see. The built-in face is the
front end's own `defaultFaceStates()`, imported rather than retyped, so a kit nobody has
designed for looks exactly like a design nobody has touched.

    design: idle: eyes=star mouth=grin brows=raised
    design: listening: eyes=star mouth=grin brows=raised
    design: thinking: eyes=star mouth=grin brows=raised
    design: speaking: eyes=star mouth=grin brows=raised
    design: talks in the 'cheeky' style
    design: kept this design (426 B)

It is kept in NVS and re-applied before the radio is even up, so a Buddy that loses
power does not come back as a stranger.

### Which face, during a conversation

A design says what each state looks like. A **`face.state`** directive says which one to
wear, and a conversation moves through them far faster than anything redraws a design:

    {"type": "face.state", "data": {"state": "listening"}}

    python3 tools/send_face.py --state thinking
    python3 tools/send_face.py --conversation     # listening -> thinking -> speaking -> idle

These are commands, not state: **never retained**, or every reconnect would replay a mood
from an exchange that finished long ago. They are not stored either — an early version kept
every one of them in NVS, which quietly overwrote the design a child had made, so the next
power cut would have lost it while the log cheerfully said it was restoring their design.

The demo cycle stops the moment anything drives the face, rather than talking over whoever
is in charge. When the voice path arrives it should drive these directly from the pipeline's
own status, which is where they will come from anyway.

**The whole catalogue is baked** — 20 eyes, 8 brows, 30 mouths, 86 sprites — because a
design arrives at runtime and may name any part in it. Baking only what the built-in
states use would mean every second face a child made fell back to a default, which is
ADR 0009's rule working correctly and looking exactly like a bug. It costs 1,725,456 B
of the 2 MB app partition; the long-term home is still the `faces` partition that ADR
0009 reserves, and this is the version that fits without it.

A part this firmware does not have falls back to what the state already showed, and
says so in the log. Motion is the one piece not applied yet: the tracks are baked, and
reading them from a design needs an interpreter that builds tracks at runtime.

    # send a face without the gateway, from a device token
    export STEM_MQTT_USER=… STEM_MQTT_PASS=…
    python3 tools/send_face.py --preset star --style cheeky
    python3 tools/send_face.py --file whatever-the-studio-saved.json

A device token may only *subscribe* to its directive topic — publishing there is the
gateway's privilege — so that bench path publishes to the device's own event topic and
needs `CONFIG_BUDDY_DESIGN_VIA_EVENT`. It reaches the same handler.

**Turn the bench path off once the gateway is doing this for real, and clear anything it
left behind.** `send_face.py` publishes retained, like the gateway, which means a test
design keeps arriving on every reconnect — and it arrives *after* the gateway's, so it
silently wins. That cost a confusing round of "the device is not updating" when the log
showed the real design applying and being overwritten 80 ms later by a week-old test.
A zero-length retained publish to the same topic is how MQTT forgets one:

    python3 tools/send_face.py --clear --no-retain   # stop sending it
    # and to delete what is already retained there, publish an empty retained payload

### The characters live in their own partition

**Three characters ship on this panel — Sprout, Pip and Leaf** — and the artwork lives in
the `faces` partition ADR 0009 reserves (**589 KB of 1,536 KB**), **memory-mapped rather
than loaded**. The pixels stay in flash and LVGL draws from where they lie, so
six characters cost **no RAM at all**. That is the only reason they are affordable, and
it is also why a new character is a catalogue image rather than an OTA for every device.

The partition image is built by an internal tool from **character artwork that is not
published** — so `face_catalogue.bin` cannot be produced from this repo, is not an output
of `idf.py build`, and is deliberately absent from the flash command in
[Flash it](#flash-it). A kit without it mounts a blank `faces` partition, says
so in the log, and draws the Buddy compiled into the firmware. Everything else works.

Flashing one, if you have built it:

    # The `faces` offset depends on which partition table you built against — the
    # tables declare sizes, not offsets, so it moves. `idf.py partition-table` prints
    # yours. It is 0x612000 with partitions-16mb.csv and 0x412000 with partitions.csv.
    esptool --chip esp32s3 -p /dev/ttyUSB0 write-flash 0x612000 build/face_catalogue.bin

Each character carries its body (RGB565A8, so it can be tinted) and the face its artist
drew — the `artwork` part ids, which are bitmaps rather than shapes anything can compute.
The drawn catalogue stays in the app: those are the same curves on every character. With
no catalogue written, the partition reads as blank flash, the mount says so, and the kit
shows the character baked into the firmware.

**The other three are deliberately not here.** Sprout, Pip and Leaf are *heads*: they
fill the frame and their features sit where `paint` says, so the baker erases the
original face and a child's chosen eyes land where the old ones were. Cardboard, Robot
and Board are objects that happen to have a face — a box on wheels, a robot with arms, a
dev board — worn as a printed sticker or a lit screen, nothing like those ellipses. On a
240 px round panel their bodies win the circle, their eyes come out a few pixels across,
and the patch misses their real features so drawn parts land beside them rather than over
them. Framing each on its head was tried and made Robot show *two* faces.

They stay on the website, where a kit card is the right place for a picture of the kit. A
kit whose design names one shows the built-in character and says so in the log. Give them
head-first artwork and they belong here too — `ON_DEVICE` in the generator is the only
thing that would change.

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
fricative is noise in both. `main/face_lipsync.c` is a transcription of the website's own
viseme classifier, and the thresholds are **generated** into
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

Two generated goldens, both emitted from the website's own TypeScript and **committed
here** so this check runs with nothing but this repo: `main/generated/xcheck.txt` for
motion sampling, `lipsync-xcheck.txt` for the classifier over
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

- ~~**Which panel?**~~ **Answered 2026-09-27**: GC9A01 240×240 round on SPI at 40 MHz, on
  the bench and drawing. The draw buffers and `partitions.csv` sizing are no longer
  provisional for this module. A *production* panel may still differ, and the one open
  request stands: **bring the backlight out to a GPIO**, which this module does not have.
- ~~**Which SoC and how much flash?**~~ **Answered on the bench**: ESP32-S3-WROOM-1-N8R2,
  8 MB flash, 2 MB embedded PSRAM — the module the hardware doc already named as the current
  choice. The 8 MB partition table is no longer a placeholder. Whether *2 MB* of PSRAM is
  enough is a voice question, not a face one, and is still open.
- **IDF version.** Pinned to v5.2.2 to match the org's existing Docker workflow. Note the
  v5.5 problem we hit elsewhere was an I²C `driver_ng` conflict raised by a peripheral
  driver this firmware does not use — so that constraint is inherited by convention here,
  not by necessity. It may well build on a newer IDF; nobody has tried.

## Design notes

The product reasoning behind this firmware — why a face is part ids rather than pixels, why
the catalogue gets its own partition, why motion is keyframes over catalogue layers — lives
in a private planning repo and is not published. The decisions it reaches are recorded in
this README where they affect the code, which is the part you need to work on it.

## Licence and support

This firmware is **Apache-2.0** — see [`LICENSE`](LICENSE). Vendored dependencies keep their
own licences and are listed in [`LICENSE-THIRD-PARTY.md`](LICENSE-THIRD-PARTY.md): LVGL 9.2
(MIT) in `components/lvgl`, and `esp_lcd_gc9a01` from esp-bsp (Apache-2.0).

The **cloud is not open source.** The gateway, the voice pipeline, the broker and the identity
service are a hosted product, and a kit uses them through a normal account. That is what the
licence covers and what it does not.

**What we answer:** this firmware building, the protocols it speaks, and a device that
provisions but will not come online. **What we cannot:** your wiring, your module, your
soldering, and boards other than the one named in [Build one yourself](#build-one-yourself).
Nothing here promises a response time.

Credentials live in `sdkconfig`, which is gitignored. **Nothing in this repo carries a token**
— if you find one, that is a bug worth reporting privately rather than in an issue.
