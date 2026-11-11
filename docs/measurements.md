# Measurements, and what only hardware found

The engineering record: every number here was taken on a real board, with the date and the build it was taken on. Read this before arguing with it.

[All docs](README.md) · [Repository README](../README.md)

---

## Status

*The S3 measurements below were taken when the S3 kit had a 2.1" 480×480 ST7701S RGB panel
with touch, before the GC9A01 replaced it (2026-10-29). The radio, voice and memory
figures carry over; the panel's own costs do not — the GC9A01 needs no PSRAM frame buffer
and no bounce buffers, and its build with voice boots with ~90 KB of internal RAM free
before Wi-Fi connects.*

**It ran on the bench board**: an ESP32-S3-WROOM-1-N16R8 driving the ST7701S 480×480
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
  clocked the high byte first, so the S3's flush swaps — see `flush_cb` in `main/main.c`.
- **240 MHz and `-O2` were never turned on.** 2.3x for no code and no memory; they are in
  `sdkconfig.defaults` now.

On the ST7701S board: the **main-task stack overflow** described under [The
gallery](gallery.md#the-gallery), and **PSRAM cannot run at 120 MHz** — the board's flash has no
high-performance mode, so it stays at 80 MHz.

### What the S3 board's bring-up found, 2026-11-03

Three things, all of them in `sdkconfig.defaults.n16r8` or `main/` now, and all three
invisible to `sim/`. (They landed in `sdkconfig.defaults.reva`, which was folded into
the N16R8 overlay on 2026-11-04 when the bench board was dropped.)

- **LVGL's 16 KB pool runs out, and running out is silent.** The build sits at
  ~11.9 KB of it with the face up and peaked at 14,000 of the 14,352 usable once the
  radio and voice were running. A sprite drawn with a transform then asks `lv_malloc()`
  for a temp buffer of up to 1,920 B (`MAX_BUF_SIZE`, 4 × 240 × 2) and does not get it —
  and `LV_ASSERT_MALLOC` fires into an `LV_ASSERT_HANDLER` that is `while(1);` by
  default, with `LV_USE_LOG` off. So the main task spins forever inside `img_draw_core`
  having printed nothing: the face freezes, the task watchdog repeats every 5 s with a
  byte-identical backtrace, and Wi-Fi, MQTT and SNTP carry on as if nothing happened.
  **Identical PC and stack pointers 25 s apart is what proves a task is wedged rather
  than slow.** `CONFIG_LV_MEM_SIZE_KILOBYTES=32` fixes it; 60 s then ran clean, with the
  pool at 11,916 of 30,580 and the peak still 14,000.
- **An idle kit's `mic:` counter never prints, and that is correct.** `up_task`'s idle
  branch ends in a `continue` before the frame counter, so a dead microphone and a quiet
  room produce byte-identical logs on the plain WakeNet path — an hour went into reading
  that silence as a fault. `main/voice.c` now meters the real microphone every 10 s
  whether or not a conversation is open (`mic idle: rms N, peak N`), the counterpart of
  the AEC build's `afe in:`. First readings: rms 169–218, peaks to 2,849 of
  32,767 in a quiet room, with WakeNet costing 3,172 µs per 32 ms chunk beside it.
- **The 60% default volume was −16 dB thrown away.** The MAX98357A has no volume control,
  so `BUDDY_SPEAKER_VOLUME` is a multiply on the samples before they reach it
  (`volume_apply` in `main/audio.c`): percent is linear in dB, 100% is untouched, every
  10% down is 4 dB. 60% is a Q8 gain of 41 against 256 — a sixth of the amplitude, and
  the quiet end of every sample given up with it. There is little to give back elsewhere:
  U601's `GAIN_SLOT` is open, which is the part's 9 dB setting and three steps below the
  15 dB it offers. The board now starts at 100%. The volume is still adjustable at runtime
  and kept in NVS, so a kit already turned down stays turned down.

### Audio, proven on hardware 2026-09-28

On the hand-wired N16R8 bench board this firmware no longer builds for, on the pin map
that went with it: the MAX98357A plays the
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

- **The S3 kit has only been run without its screen.** On 2026-10-29 an S3 N16R8 with
  nothing attached booted the GC9A01 build cleanly — panel driver up over SPI, the three
  characters mapped, the I2S bus up with voice on, Bluetooth setup advertising, memory
  flat over a minute. What that cannot show: a picture on the screen, the idle clock (it
  needs the network for the time), and "Jarvis" bringing the face back. The 240 px face
  and the clock have been seen on glass since, on rev A (2026-11-03 and 2026-11-04); the
  setup screen is still sized by arithmetic only, because reaching it erases the kit's
  Wi-Fi credentials.
- **Voice sessions drop on a congested Wi-Fi network.** Seen on the P4: a conversation
  closes a second or two after it opens, and the status cycles *connecting → listening*.
  Every WebSocket send in `main/voice.c` waits at most 200 ms, and `esp_websocket_client`
  treats one write that takes longer as a dead connection. Raising the limit is not a
  one-line fix: the 200 ms keeps the microphone task inside its 20 ms budget.
- **Interrupting the Buddy, on the S3, now has a hardware answer.** BTN2 raises the
  same signal the wake word raises, so a press cuts a reply off at any volume
  ([voice.md](voice.md)). It builds for both boards and **has not been pressed on a
  board yet**. What it does not solve is below: interrupting by *voice* alone.
- **Interrupting the Buddy without the wake word, on the S3.** One microphone and no
  echo cancellation leaves nothing to subtract the speaker from, so the uplink is muted
  while the Buddy speaks and "Jarvis" is the only way in. Every partial setting tried
  either let the Buddy interrupt itself or stopped a child interrupting it. The P4
  answered this with the AFE rather than with settings — echo cancelled, microphone open,
  talk over it as you would in a browser ([Echo
  cancellation](voice.md#echo-cancellation-and-the-microphone-slots)).
- **BOOT as a wake pin on the S3** is not confirmed on hardware. GPIO 0 is a strapping pin,
  so a press might land in the bootloader instead of waking the kit. It matters only for
  the explicit `device.sleep` command now that idle shows a clock — see [Deep
  sleep](idle-and-sleep.md#deep-sleep-and-why-idle-no-longer-reaches-it).
