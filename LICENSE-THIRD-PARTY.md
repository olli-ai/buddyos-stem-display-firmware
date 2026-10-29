# Third-party licences

This firmware is Apache-2.0 ([`LICENSE`](LICENSE)). The components below are vendored
under `components/` or fetched before a build, and each keeps its own licence — the full
text sits in that component's directory.

**The Buddy characters are Apache-2.0 too.** The artwork in `faces/face_catalogue.bin`
(Sprout, Pip and Leaf) and the built-in face and parts in `main/generated/face_assets_*.c`
are BuddyOS STEM's own and are covered by [`LICENSE`](LICENSE) like the code — use,
modify and redistribute them on the same terms. They are not third-party, and appear here
only so nobody has to guess.

| Component | Version | Licence | Upstream |
| --- | --- | --- | --- |
| `lvgl` (not committed — cloned before the first build) | 9.2 | MIT | <https://github.com/lvgl/lvgl> |
| `esp_lcd_gc9a01` | — | Apache-2.0 | <https://github.com/espressif/esp-bsp> |
| `esp_lcd_jd9365` | 2.0.2 | Apache-2.0 | <https://github.com/espressif/esp-iot-solution> |
| `esp_codec_dev` | 1.5.11 | Apache-2.0 | <https://github.com/espressif/esp-adf> |
| `esp_hosted` | 1.4.7 | Apache-2.0 | <https://github.com/espressif/esp-hosted-mcu> |
| `esp_wifi_remote` | 0.14.5 | Apache-2.0 | <https://github.com/espressif/esp-wifi-remote> |
| `esp_websocket_client` | 1.8.0 | Apache-2.0 | <https://github.com/espressif/esp-protocols> |
| `esp-dsp` | 1.6.0 | Apache-2.0 | <https://github.com/espressif/esp-dsp> |
| `dl_fft` | 0.6.0 | MIT (declared in its `idf_component.yml`) | <https://github.com/espressif/esp-dl> |
| `esp-sr` | 2.4.7 | **Espressif MIT** | <https://github.com/espressif/esp-sr> |
| `esp_audio_codec` | 2.6.2 | **Espressif Modified MIT** | <https://github.com/espressif/esp-adf-libs> |

`esp_hosted` also carries `protobuf-c` (BSD-2-Clause, `components/esp_hosted/common/protobuf-c/LICENSE`),
and `esp_codec_dev` carries a Microsemi ZL38063 driver under its own licence
(`components/esp_codec_dev/device/zl38063/LICENSE.txt`).

## The two Espressif-only licences

`esp-sr` (the WakeNet model behind "Jarvis", and the audio front end) and `esp_audio_codec`
are free to use, modify and redistribute **only on Espressif chips**. That covers both
boards this firmware builds for (ESP32-S3 and ESP32-P4). It does not cover porting that
code, or its models, to another vendor's silicon. Everything else in this repository has
no such restriction.
