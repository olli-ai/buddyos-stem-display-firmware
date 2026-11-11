<div align="center">

<img src="docs/media/face.gif" alt="The Buddy face: idle, listening, speaking with lip-sync, thinking" width="320">

# An open-source robot face

**ESP-IDF firmware for a round screen that looks back at you.** A 1.28" GC9A01 on an
ESP32-S3 — or a 4" MIPI-DSI panel on an ESP32-P4 — running an animated LVGL face with
on-device wake-word detection and a streaming voice assistant behind it.

[![Licence](https://img.shields.io/badge/licence-Apache--2.0-blue.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5.3-red.svg)](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/)
[![Boards](https://img.shields.io/badge/boards-ESP32--S3%20%7C%20ESP32--P4-green.svg)](docs/hardware.md)

[Getting started](GETTING_STARTED.md) · [Documentation](docs/README.md) ·
[Wiring](docs/hardware.md) · [Releases](https://github.com/olli-ai/buddyos-stem-display-firmware/releases)

</div>

---

The face above is the firmware rendering itself — same `face_view.c`, same motion
interpreter, same lip-sync classifier the device runs, drawn by the headless simulator
in [`sim/`](sim/). It blinks, breathes, moves its brows, follows the audio it is
speaking with its mouth, and changes expression when the model it is talking to asks it
to.

**Every part is an off-the-shelf breakout.** A GC9A01 module, an INMP441 microphone, a
MAX98357A amplifier and a devkit on a breadboard run exactly what our PCB runs — one
pin map, one branch, one config flag's difference. See [wiring](docs/hardware.md).

## Try it

```sh
git clone https://github.com/olli-ai/buddyos-stem-display-firmware.git
cd buddyos-stem-display-firmware

# LVGL is not committed — clone it once, before the first build
git clone --depth 1 --branch release/v9.2 https://github.com/lvgl/lvgl.git components/lvgl

# ESP32-S3 (the N16R8 overlay is NOT applied automatically)
idf.py -B build-s3 -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-s3/sdkconfig \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.n16r8" build

# ESP32-P4 — IDF layers sdkconfig.defaults.esp32p4 on its own
idf.py -B build-p4 -DIDF_TARGET=esp32p4 -DSDKCONFIG=build-p4/sdkconfig build
```

Then flash with `esptool write-flash "@flash_args"` from inside the build directory —
never offsets typed by hand. ESP-IDF **v5.5.3**; no toolchain installed? Use
`espressif/idf:v5.5.3`. The whole path, including the words to hand an AI assistant so
it does the installing and flashing for you, is in
**[Getting started](GETTING_STARTED.md)**; the detail is in
**[docs/building.md](docs/building.md)**.

**No board yet?** `bash sim/build.sh --serve` renders the face in your browser at
`localhost:8080` with nothing plugged in.

## What it does

| | |
| --- | --- |
| **Animated face** | Seven states, drawn from vector art baked per panel. Blinks, breathes, brows, head turns; expressions a model can change mid-sentence; a `.buddy` motion interpreter for scripted moves. [docs/face.md](docs/face.md) |
| **Hears its name** | ESP-SR WakeNet on the device. **Nothing leaves the board until it fires** — no audio, no transcript. [docs/voice.md](docs/voice.md) |
| **Talks** | Opus over a WebSocket, 20 ms frames both ways, barge-in, and a mouth that follows the audio actually being played rather than the text. |
| **Button** | BTN2 on the S3 does what the wake word does — press, let go, it listens. It interrupts at any volume, which the wake word cannot. |
| **Sets itself up** | BLE provisioning from a browser, serial and QR on the glass, then MQTT over TLS. [docs/setup.md](docs/setup.md) |
| **Knows when it is alone** | After three quiet minutes the face gives way to a clock, and the kit stays listening. [docs/idle-and-sleep.md](docs/idle-and-sleep.md) |
| **Or shows photographs** | A second build: your photos, one swipe apart, with the jaw and eyes of a still photograph puppeted. [docs/gallery.md](docs/gallery.md) |

## Two boards, one branch

`IDF_TARGET` picks everything — panel driver, audio driver, face bake. There is no flag
to set and no branch to switch.

| Board | Target | Screen | Audio |
| --- | --- | --- | --- |
| **ESP32-S3-WROOM-1-N16R8** | `esp32s3` | GC9A01 1.28" 240×240 round, SPI, no touch | INMP441 + MAX98357A, no echo cancelling |
| **ESP32-P4** (e.g. Waveshare ESP32-P4-WIFI6-Touch-LCD-4C) | `esp32p4` | JD9365 4" 720×720 MIPI-DSI, touch | ES7210 + ES8311, echo cancelling on |

## What needs the cloud, and what does not

Worth knowing before you build it, because it is the question this README used to bury.

- **On its own**, flashed and unprovisioned: it boots, draws the face, runs the motion
  and expression cycle, and renders the setup screen. The simulator needs nothing at all.
- **The voice assistant, the face designer and sending it programs** go through the
  BuddyOS cloud, which is **a hosted product and not open source** — a kit uses it
  through an ordinary account at [app.buddystem.com](https://app.buddystem.com). The
  firmware is free; the servers that listen and answer are the paid part.
- **The protocols are all here.** The MQTT topics, the directive schema and the voice
  WebSocket are described in these docs, and nothing stops a different server speaking
  them.

## What this repo is *not*

A gallery, a voice and a screen — nothing else. The only inference on the device is the
wake word, in its own partition. No camera, no offline vision model, no training. The
[measurements](docs/measurements.md) are the argument for that line, not a preference.

## Status, honestly

Both boards run. The S3's face, clock, voice path and provisioning have been exercised on
real hardware; the P4 runs current `master`. Everything that has **not** been confirmed on
a board is marked as such where it is claimed — including, right now, the BTN2 talk button,
which builds and has never been pressed. See
[open questions](docs/measurements.md#open-questions).

Every number in these docs was measured on a board, with the date and build it came from.
If your board disagrees, your board is right.

## Documentation

**[docs/README.md](docs/README.md)** is the index — building, hardware and wiring, setup,
voice, the face, idle behaviour, the gallery, the P4 board, and the measurements. It also
says, for an AI assistant working in this repository, which page answers what and which
files are the source of truth.

## Licence

**Apache-2.0** ([`LICENSE`](LICENSE)) — and that includes the Buddy artwork in `faces/`
and the built-in face. Vendored dependencies keep their own licences
([`LICENSE-THIRD-PARTY.md`](LICENSE-THIRD-PARTY.md)): mostly Apache-2.0 and MIT, except
`esp-sr` (the wake word) and `esp_audio_codec`, which are Espressif's and allow use only
on Espressif chips.

**What we can help with:** this firmware building, the protocols it speaks, and a device
that provisions but will not come online — open an issue with the serial log attached.
**What we cannot:** your wiring, your modules, your soldering, and boards other than the
two above. Nothing here promises a response time.

Credentials live in `sdkconfig`, which is gitignored. **Nothing in this repo carries a
token** — if you find one, report it privately rather than in an issue.
