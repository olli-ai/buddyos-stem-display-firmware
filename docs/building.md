# Building and flashing

Which board you have, how to build for it, and how to write the result to flash. Start at [Getting started](../GETTING_STARTED.md) if you would rather an assistant did it.

[All docs](README.md) · [Repository README](../README.md)

---

## Which board

Two boards carry this device type, and **one branch builds both**. There is no flag to
set: `IDF_TARGET` decides, and with it the panel driver, the audio driver and the face
artwork `main/CMakeLists.txt` compiles.

| Your board | Target | Screen | Build directory |
| --- | --- | --- | --- |
| **ESP32-S3 board** (ESP32-S3-WROOM-1-N16R8, laid out) | `esp32s3` | 1.28" 240x240 round GC9A01, SPI, **no touch** | `build-s3` |
| **ESP32-P4** (32 MB flash), e.g. Waveshare ESP32-P4-WIFI6-Touch-LCD-4C | `esp32p4` | 4" 720x720 JD9365 MIPI-DSI, touch | `build-p4` |

**There is one S3 pin map**, the laid-out board's — and **you can wire it yourself**.
The parts are all common breakouts, so a GC9A01 module, an INMP441 and a MAX98357A on
the same GPIOs run the same firmware; see [Wiring](hardware.md#wiring) for the table. The older
hand-wired bench board used *entirely different* pins and was dropped on 2026-11-04,
along with the `BUDDY_S3_BOARD` choice and `sdkconfig.defaults.reva` (whose settings
moved into `sdkconfig.defaults.n16r8`). What went away was that second pin map, not the
idea of wiring one up.

**One flag to flip if you do.** `BUDDY_S3_PANEL_MIRROR_X` is on, because the board we
ship mounts its panel the other way round through its FPC. A breakout is mounted the
normal way, so leave it on and the face comes out backwards:

```
CONFIG_BUDDY_S3_PANEL_MIRROR_X=n
```

That is the whole difference. The face is not turned on either board
(`BOARD_FACE_ROT_DEG` is 0 since 2026-11-05), so nothing else about a hand-wired build
differs from the one we ship.

Both build with **ESP-IDF v5.5.3**. Older IDF does not build this tree (the P4's panel
driver needs 5.4 or later). No IDF installed? Use Espressif's image,
`espressif/idf:v5.5.3` — see [Building](building.md#building).

```sh
git clone https://github.com/olli-ai/buddyos-stem-display-firmware.git
cd buddyos-stem-display-firmware

# LVGL is not committed — clone it once, before the first build
git clone --depth 1 --branch release/v9.2 https://github.com/lvgl/lvgl.git components/lvgl

# ESP32-S3 board — the N16R8 overlay is not applied automatically
idf.py -B build-s3 -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-s3/sdkconfig \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.n16r8" build

# ESP32-P4 — IDF layers sdkconfig.defaults.esp32p4 on its own
idf.py -B build-p4 -DIDF_TARGET=esp32p4 -DSDKCONFIG=build-p4/sdkconfig build
```

**The S3 kit has no touch, so it builds with voice on.** After three quiet minutes the face
gives way to a clock, and on this kit "Jarvis" is what brings it back — there is no screen
to tap. `sdkconfig.defaults.n16r8` turns voice on for it.

**Both boards build with voice on**, and neither needs a flag for it: the S3's overlay
sets it, and `sdkconfig.defaults.esp32p4` does the same for the P4.

`BUDDY_AUDIO`'s Kconfig default is still `n`, and its help still says why — bringing a
board's audio up the first time once hung a device past the ROM bootloader. That is a
one-time gate per board, and both have passed it. It was left shut on the P4 for weeks
after that stopped being true, which cost a step in the public setup guide, a paragraph
here, and the odd kit built from plain defaults that could not hear a thing. The default
is for the next board, not for either of these.

Everything downstream already defaults to what the bench measured — echo cancellation on
for the P4, and 30 dB of uplink gain, without which the cloud's VAD never hears a voice
and the kit cannot be interrupted.

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
area and each panel needs its own bake — 240x240 for the S3, 720x720 for the P4 (~4 MB). Both are committed and exactly one is compiled, which is why building needs
nothing but ESP-IDF. `face_view.c` takes the bake's dimensions *as* the panel's, so a
mismatched one does not scale to fit; it draws short of the edges. The P4 additionally
compiles `face_ppa.c`, its 2D accelerator, and its own panel and audio drivers.

The S3 and the P4 took separate branches for a while, when the P4's bake was the only
one and it did not fit the S3's app slot. They no longer do, and fixes land once.

## Building

Build with **ESP-IDF v5.5.3**. There is no Dockerfile: Espressif's own image is the build
environment, and a native install of the same version works identically. Older IDF does
not build this tree — the P4's panel driver needs 5.4 or later.

Dependencies are **vendored in `components/`**, so the component manager is switched off
and nothing is fetched from the registry. Clone LVGL first (below), then, from the repo
root:

    # ESP32-S3 board
    docker run --rm -e IDF_COMPONENT_MANAGER=0 -v "$PWD":/fw -w /fw espressif/idf:v5.5.3 \
        idf.py -B build-s3 -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-s3/sdkconfig \
               -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.n16r8" build

    # ESP32-P4
    docker run --rm -e IDF_COMPONENT_MANAGER=0 -v "$PWD":/fw -w /fw espressif/idf:v5.5.3 \
        idf.py -B build-p4 -DIDF_TARGET=esp32p4 -DSDKCONFIG=build-p4/sdkconfig build

Keep the `-B` and `-DSDKCONFIG` pair. Without them both boards share `build/` and the root
`sdkconfig`, and an `sdkconfig` left over from one board, or from an older checkout,
quietly wins over the defaults files — a build for the panel you do not have.

The container cannot see a USB port, so flash from the host ([Flash it](building.md#flash-it)).

`components/lvgl` is not committed. Clone it once, before the first build:

    git clone --depth 1 --branch release/v9.2 https://github.com/lvgl/lvgl.git components/lvgl

A gallery build (`CONFIG_BUDDY_SCREEN_GALLERY`) also needs `gallery/gallery.bin` baked
first ([The gallery](gallery.md#the-gallery)); the face build carries its art in `face_assets.c` and
needs no photos.

The panel driver glue is written here rather than pulled from `esp_lvgl_port`: on the S3 a
display, two partial draw buffers in DMA-capable internal RAM, a flush that byte-swaps and
hands the area to SPI DMA (`main/gc9a01.c`), and a tick from `esp_timer`. One fewer
dependency.

### Flash it

Build first ([Building](building.md#building)) — one build directory per board, `build-s3` or
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
the kit keeps its Wi-Fi and account setup, so a reflashed kit comes back already set up.
It does write `faces`, the character catalogue
([The characters](face.md#the-characters-and-why-they-are-baked-at-384)), from the copy in this
repo. **`esptool erase-flash` removes the setup**: the kit returns to setup. To clear just
the setup without erasing anything else, hold BOOT for 5 s instead
([Setting a kit up](setup.md#setting-a-kit-up)).

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
to [Setting a kit up](setup.md#setting-a-kit-up).

### Flashing the N16R8 board

It uses `partitions-16mb.csv`: two 4.5 MB app slots, then `faces` (the character
catalogue) and `model` (the wake word). **After the partition table changes, flash all the
images** — the app alone leaves the old table in place and the kit looks for its model and
characters where they no longer are. The command is in [Flash it](building.md#flash-it).
