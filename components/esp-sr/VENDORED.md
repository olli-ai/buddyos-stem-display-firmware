# esp-sr — vendored

ESP-SR **2.4.7** from the Espressif component registry
(`espressif/esp-sr`), copied in because this project builds with
`IDF_COMPONENT_MANAGER=0`. Its dependencies are vendored or supplied the same way:

- `dl_fft` 0.6.0 — `../dl_fft`, whole, from the registry. Replaced kissfft inside the
  libraries from 2.3.0.
- `esp-dsp` — `../esp-dsp`, still 1.6.0. Upstream asks for 1.8.0; the symbols these
  libraries call are all in 1.6.0, and the link says so if that ever stops being true.
- `cjson` — **not** vendored. IDF's own `json` component is the same cJSON 1.7.19, and
  linking both would define every `cJSON_*` twice.

2.4.7 is the newest release that does not also need `esp-dl` (2.5.0 onwards).

## Why not the 2.1.2 this repo first carried

The Waveshare board's ESP32-P4 is silicon **v3.2**. v3 changed the encoding of the PIE
(vector) instructions, and 2.1.2's `lib/esp32p4` was built for the older chips: WakeNet's
first vector copy, `dl_esp32p4_memcpy` during model load, raised `MCAUSE 0x1f` and the kit
rebooted every time Wi-Fi came up and the voice session started. 2.4.1 was the first
release built for both; 2.4.5 also fixed a P4 crash on IDF 5.5.1/5.5.2.

Upstream ships three P4 builds: `esp32p4` (v3, IDF ≥ 5.5.3), `esp32p4_less_v3` and
`esp32p4_less_v3_idf6`. Only `esp32p4` is vendored, which matches
`CONFIG_ESP32P4_REV_MIN_300` in `sdkconfig.defaults.esp32p4` on IDF 5.5.5;
`CMakeLists.txt` refuses to configure a pre-v3 P4 build rather than link the wrong set.

## What is kept

- the `esp32s3` and `esp32p4` headers and libraries;
- `src/model_path.c` (model loading) and `src/esp_sr_debug.c` (WakeNet links against
  its debug switch);
- `model/movemodel.py` + `pack_model.py`. One edit to `movemodel.py`: its report's rule
  lines are ASCII `-` instead of U+2500. On a Windows console the build's Python writes
  cp1252, which cannot encode U+2500, so the packing step failed and the build failed
  with it;
- three wake-word models: `wn9_hiesp` ("Hi ESP"), `wn9_jarvis_tts` ("Jarvis"),
  `wn9_hijoy_tts` ("Hi Joy").

Per target, the libraries are WakeNet and what it links against, plus the audio front
end. On the P4 that is the whole AFE (`esp_afe_sr_iface.h`: echo cancellation feeding
WakeNet), which links against `libnsnet.a` and `libvadnet.a` even with noise suppression
and VAD left off. On the S3 they are in the link group too, and nothing pulls them in.
`CMakeLists.txt` is rewritten for that subset.

## What is removed

- every other target;
- MultiNet (speech commands) — `libmultinet.a`, `libfst.a`, `libflite_g2p.a` — and its
  models;
- the Chinese TTS;
- the other wake words, tests, docs.

To use another registry wake word, copy its `model/wakenet_model/<name>` directory from
the same release and select it in menuconfig. `movemodel.py` silently packs nothing for a
model whose directory is missing, so the device then boots with no wake word.

No "Hi Buddy" model exists: Espressif trains a custom wake word on request.

## The P4 board's other components

The P4 board's other dependencies are vendored the same way, each at its registry version
with its `idf_component.yml` kept for reference:

| Component | Version | Provides |
|---|---|---|
| `esp_codec_dev` | 1.5.11 | ES8311, ES7210 |
| `esp_lcd_jd9365` | 2.0.2 | the panel |
| `esp_hosted` | 1.4.7 | Wi-Fi and BLE from the C6 |
| `esp_wifi_remote` | 0.14.5 | Wi-Fi and BLE from the C6 |

One edit to `esp_hosted`: `host/drivers/transport/transport_drv.c` asserted when its
TX buffer pool and the DMA heap behind it were both empty. On a slow link mid-
conversation that rebooted the kit (2026-10-21); it now returns the netif's TX
error, lwIP drops the frame, and TCP sends it again.

The top-level `CMakeLists.txt` builds only what `main` asks for, so none of them is
compiled for the S3.

Licence: Espressif MIT (`LICENSE`) — free to use on Espressif chips only, which
this device is. `dl_fft` is MIT.
