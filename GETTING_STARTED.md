# Getting started

From a board in your hand to a Buddy that answers "Jarvis", in four steps. You do not
need to know ESP-IDF: an AI coding assistant can install the tools, build the firmware
and flash it for you, and this page gives you the exact words to ask it with.

If you want the engineering detail instead, read [`README.md`](README.md).

---

## 1. Get a board

There is **no wiring step**. Both boards this firmware builds for come assembled, and
it knows one pin map per board — there is nothing to choose and nothing to connect.

| Board | What it is |
| --- | --- |
| **BuddyOS STEM display kit** | The ESP32-S3 board we ship: an ESP32-S3-WROOM-1-N16R8, a GC9A01 1.28" round screen, an INMP441 microphone and a MAX98357A amplifier, all on the PCB. |
| **Waveshare ESP32-P4-WIFI6-Touch-LCD-4C** | An off-the-shelf 4" 720×720 touch board you can buy today. |

You also need a **USB cable that carries data** — many only charge, and the computer
will not see the board at all.

> **Wiring the parts up yourself on a breadboard is no longer supported.** The firmware
> carried a hand-wired pin map until 2026-11-04 and no longer does: it bakes in the
> shipped board's GPIOs, its panel mirror and the 48° turn its enclosure needs. Point it
> at a bare module and the screen comes up mirrored and tilted with every wire right.
> The old map is in git history if you want it.

<details>
<summary><b>What the ESP32-S3 board already has wired</b> — nothing to connect, but useful when you are reading a log, writing a program or probing the board</summary>

ESP32-S3 GPIO numbers. `main/board_pins.h` is the source of truth; if this table and that
file ever disagree, the file is right.

| Part | Signal | GPIO | Notes |
| --- | --- | --- | --- |
| **GC9A01** screen (SPI) | `SCL` | 12 | through R501 |
| | `SDA` | 11 | through R502 |
| | `DC` | 16 | |
| | `CS` | 17 | R503 pull-up |
| | `RST` | 18 | R504 pull-up |
| | backlight | 13 | Q501's gate — high lights it |
| **MAX98357A** amp (I2S0, out) | `BCLK` | 47 | |
| | `LRC` | 48 | |
| | `DIN` | 21 | ESP32 → amplifier |
| | `SD_MODE` | 14 | high enables, low mutes |
| **INMP441** mic (I2S1, in) | `SCK` | 38 | |
| | `WS` | 39 | |
| | `SD` | 40 | microphone → ESP32 |
| Buttons and the rest | BTN1 / BOOT | 0 | hold 2 s to re-run setup |
| | BTN2 | 1 | unused by the firmware; silkscreen says "IO21", it is IO1 |
| | battery sense | 8 | ADC1_CH7, 100k/100k divider |
| | WS2812 | 2 | footprint only, not fitted |

- The speaker and the microphone are on **two separate I²S controllers**, each with its
  own clocks — not the shared bus most online GC9A01 guides use.
- The amplifier's `GAIN_SLOT` is open, so the gain is a fixed **9 dB**; the mic's `L/R`
  is tied to GND, so it always talks in the **left** half of the frame.
- **The speaker has no connector** — it solders to test points **TP504**/**TP505**. A
  board with the amplifier fitted and nothing on those pads is silent with every
  register correct.
- GPIO 35–37 (octal PSRAM), 19/20 (native USB), 26–32 (flash) and the strapping pins
  3/45/46 are left alone.

</details>

## 2. Let an assistant build and flash it

Install one of these on your computer:

- **Claude Code** — <https://claude.com/claude-code>
- **Codex CLI** — <https://github.com/openai/codex>

Then download this repository, open a terminal **in its folder**, start the assistant
(`claude` or `codex`), plug the board in with your data cable, and paste this — after
changing the two lines in brackets:

```text
I want to build and flash the BuddyOS STEM display firmware in this folder.
My board is: [BuddyOS STEM display kit (ESP32-S3) | Waveshare ESP32-P4]
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

## 3. Set it up on the website

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

## 4. Talk to it

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
| The screen stays dark | Ask the assistant for the serial line `gc9a01: 240x240 on SPI`. If it is there the panel is talking, and the backlight is the suspect. |
| The kit is not in the Bluetooth list | It is already set up. Hold BOOT for 2 s to put it back in setup. Or the browser is not Chrome/Edge. |
| "Device registration failed" | Ask the assistant to check the log for `mqtt connected` — it shows whether the kit reached the cloud. Make sure the Wi-Fi is 2.4 GHz. |
| It never hears "Jarvis" | Ask the assistant for the `mic idle: rms N, peak N` line the kit prints every 10 s. `rms 0, peak 0` is a dead microphone; a quiet room reads a few hundred. |
| It goes silent or restarts when it speaks | The 5 V rail is sagging under the amplifier. Try another cable, or a port that supplies more current. |

Still stuck? Open an issue on this repository with the serial log attached.
