# Getting started

From a bag of parts to a Buddy that answers "Jarvis", in five steps. You do not need
to know ESP-IDF: an AI coding assistant can install the tools, build the firmware and
flash it for you, and this page gives you the exact words to ask it with.

If you want the engineering detail instead, read [`README.md`](README.md).

---

## 1. Get the parts

| Part | What to buy |
| --- | --- |
| Board | **ESP32-S3-WROOM-1-N16R8** devkit (16 MB flash, 8 MB PSRAM). **Not the N8R2.** |
| Screen | **GC9A01** 1.28" round TFT module, 240x240, SPI |
| Microphone | **INMP441** I²S microphone breakout |
| Amplifier | **MAX98357A** I²S amplifier breakout |
| Speaker | small 4 Ω speaker, 2–3 W |
| Cable | a USB cable that carries **data** — many only charge |

Have a **Waveshare ESP32-P4-WIFI6-Touch-LCD-4C** instead? It needs no wiring at all:
skip to step 3 and tell the assistant your board is the P4.

## 2. Wire it

Breadboard and jumper wires, no soldering beyond the headers. The pin numbers are the
ESP32-S3's GPIO numbers, printed on most devkits.

| Wire from | To GPIO |
| --- | --- |
| Screen `SCL` | 15 |
| Screen `SDA` | 16 |
| Screen `DC` | 17 |
| Screen `CS` | 18 |
| Screen `RST` | 8 |
| Screen `VCC`, `BLK` | 3V3 |
| Mic `SCK` **and** amp `BCLK` (both to the same pin) | 5 |
| Mic `WS` **and** amp `LRC` (both to the same pin) | 6 |
| Amp `DIN` | 7 |
| Mic `SD` | 4 |
| Mic `L/R` | GND |
| Mic `VDD` | 3V3 |
| Amp `VIN` | **5 V** |
| Every `GND` | GND |

Two things people get wrong:

- **The mic and the amp share two wires** (GPIO 5 and 6). That is on purpose.
- **The amp runs from 5 V**, not 3V3. On 3V3 the board browns out the moment it speaks
  and looks broken when it is only starved.

## 3. Let an assistant build and flash it

Install one of these on your computer:

- **Claude Code** — <https://claude.com/claude-code>
- **Codex CLI** — <https://github.com/openai/codex>

Then download this repository, open a terminal **in its folder**, start the assistant
(`claude` or `codex`), plug the board in with your data cable, and paste this — after
changing the two lines in brackets:

```text
I want to build and flash the BuddyOS STEM display firmware in this folder.
My board is: [ESP32-S3-WROOM-1-N16R8 with a GC9A01 screen | Waveshare ESP32-P4]
My computer runs: [macOS | Windows | Linux]

Please:
1. Read GETTING_STARTED.md, then README.md's "Which board", "Building" and "Flash it".
2. Check what I already have (ESP-IDF v5.5.3, or Docker; Python; git) and install only
   what is missing. Ask me before installing anything large.
3. Clone LVGL into components/lvgl exactly as the README says.
4. Build with the README's command for my board, in that board's own build directory.
5. Find my board's serial port and run `esptool chip-id`. Stop and tell me if the chip
   is not the one I named.
6. Flash with `esptool write-flash "@flash_args"` from inside the build directory.
   Do not type flash offsets by hand. Do not run erase-flash.
7. Open the serial log and tell me whether it booted, using the list under
   "Watch it boot" in the README.
Before each step, tell me in one sentence what you are about to do.
```

The assistant will ask permission before running commands. Read what it says and say
yes when it makes sense. The first build takes a few minutes.

**When it is done, the screen shows "Set this kit up"**, a name like
`ESP32-cc8da2ece434`, and a QR code. That name is your kit's serial — write it down.

<details>
<summary>Prefer to do it yourself?</summary>

Install **ESP-IDF v5.5.3** (or Docker), then from this folder:

```sh
git clone --depth 1 --branch release/v9.2 https://github.com/lvgl/lvgl.git components/lvgl
idf.py -B build-s3 -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-s3/sdkconfig \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.n16r8" build
python3 -m venv venv && venv/bin/pip install esptool
cd build-s3 && ../venv/bin/esptool --chip esp32s3 -p PORT write-flash "@flash_args"
```

The P4 and Docker commands are in the README under [Building](README.md#building).
</details>

## 4. Set it up on the website

You need **Chrome or Edge** on a computer or an Android phone — they are the browsers
that can talk Bluetooth to the kit. Safari, Firefox and iPhones cannot.

1. Go to **<https://app.buddystem.com>** and sign in (or create an account).
2. Add a device, and choose to set it up with Bluetooth.
3. Pick your kit from the list — it has the name the screen shows.
4. Choose your Wi-Fi and type its password. **2.4 GHz networks only**: the kit cannot
   see 5 GHz ones.
5. Wait for "registered". It usually takes a few seconds.

Your kit now appears in your device list. From there you can design its face, give it a
voice, and send it programs.

## 5. Talk to it

- **Say "Jarvis"**, wait for the face to react, then talk. It answers out loud.
- **Leave it alone for three minutes** and the face turns into a clock. Say "Jarvis" to
  bring the face back.
- **To set it up again** (new Wi-Fi, new owner): hold the **BOOT** button for 2 seconds
  and let go. The setup screen comes back. Deleting the kit on the website does the same.

---

## When something goes wrong

Paste this into the assistant, with your problem in the brackets:

```text
My BuddyOS STEM display kit has a problem: [describe what you see].
Open the serial log without resetting the board, read it, and tell me what it says is
wrong. Use README.md and GETTING_STARTED.md to explain the fix. Do not reflash or erase
anything until I agree.
```

Common ones:

| What you see | What it usually is |
| --- | --- |
| The computer does not see the board at all | A charge-only USB cable, or a hub or adapter that passes power but not data. Plug straight into the computer with another cable. |
| esptool cannot connect | Hold **BOOT**, press and release **RESET**, let go of BOOT, and try again. |
| The screen stays dark | Check the five screen wires and `VCC`/`BLK` against step 2. |
| The kit is not in the Bluetooth list | It is already set up. Hold BOOT for 2 s to put it back in setup. Or the browser is not Chrome/Edge. |
| "Device registration failed" | Ask the assistant to check the log for `mqtt connected` — it shows whether the kit reached the cloud. Make sure the Wi-Fi is 2.4 GHz. |
| It never hears "Jarvis" | The mic wiring — mic `SCK`/`WS` must share GPIO 5/6 with the amp. |
| It goes silent or restarts when it speaks | The amp is on 3V3. Move it to 5 V. |

Still stuck? Open an issue on this repository with the serial log attached.
