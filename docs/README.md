# Documentation

Everything that used to be one very long `README.md`, split by subject. Nothing was
rewritten in the move: these pages are the same engineering record, with the same
measurements and the same dates.

**Start here if you are holding a board:** [Getting started](../GETTING_STARTED.md).

## The pages

| Page | What it answers |
| --- | --- |
| [building.md](building.md) | Which board you have, the exact build commands per target, flashing with `@flash_args`, and what a healthy boot logs. |
| [hardware.md](hardware.md) | What is on the PCB, the full GPIO table, wiring your own from breakouts, why the module is the N16R8 and why the amplifier needs 5 V. |
| [setup.md](setup.md) | BLE provisioning, what the setup screen shows, re-running setup, and what a kit needs from the cloud. |
| [voice.md](voice.md) | The wake word, what leaves the device and when, half duplex on the S3, the talk button, echo cancellation and the microphone slots. |
| [face.md](face.md) | How the face is baked per panel, catalogue characters, the motion interpreter, expressions chosen by the model, and the skills a program can call. |
| [idle-and-sleep.md](idle-and-sleep.md) | The clock after three quiet minutes, and why idle no longer ends in deep sleep. |
| [gallery.md](gallery.md) | The other build: photographs instead of a face, and how to make a photograph blink and talk. |
| [p4.md](p4.md) | The Waveshare ESP32-P4 board — its panel, its touch layer, its radio, and setting a kit up by camera. |
| [measurements.md](measurements.md) | Memory and timing taken on real boards, what only hardware found, what TLS costs, how to repeat a measurement, and the open questions. |

## For an AI assistant reading this repository

Read in this order, and stop when the question is answered:

1. **[../GETTING_STARTED.md](../GETTING_STARTED.md)** — the whole happy path, board to
   conversation. Most tasks need nothing else.
2. **[building.md](building.md)** — before running any build or flash command. The `-B`
   and `-DSDKCONFIG` pair is not optional, and offsets are never typed by hand.
3. The page above whose row matches the question.

Three rules that save a session:

- **`main/board_pins.h` is the source of truth for every pin.** Tables in these pages
  are copies, kept in step by hand; the header wins any disagreement.
- **`main/Kconfig.projbuild` is the source of truth for every `CONFIG_BUDDY_*`**, and
  each one's `help` says why it defaults the way it does. Read the help before changing
  a default.
- **Measurements carry their date and their board.** If a number here disagrees with a
  board on your desk, the board is right and the number is old — say so rather than
  explaining the board away.

What is deliberately **not** here: nothing in this repository talks to the camera, runs
an offline vision model, or trains anything. That is a different device.
