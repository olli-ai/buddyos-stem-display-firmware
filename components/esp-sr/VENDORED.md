# esp-sr — vendored

ESP-SR **2.1.2** from the Espressif component registry
(`espressif/esp-sr`), copied in because this project builds with
`IDF_COMPONENT_MANAGER=0`. 2.1.2 is the newest release whose only dependency is
esp-dsp (vendored alongside as `../esp-dsp`, 1.6.0); from 2.1.3 it also needs
`dl_fft`, and from 2.4.x `cjson` and later `esp-dl`.

Kept: the esp32s3 headers, `src/model_path.c` (model loading), `src/esp_sr_debug.c`
(WakeNet links against its debug switch), the six prebuilt
libraries WakeNet links against, `model/movemodel.py` + `pack_model.py`, and three
wake-word models — `wn9_hiesp` ("Hi ESP"), `wn9_jarvis_tts` ("Jarvis"),
`wn9_hijoy_tts` ("Hi Joy"). `CMakeLists.txt` is rewritten for that subset.

Removed: every other target, MultiNet (speech commands) and its models, the
Chinese TTS, nsnet/vadnet, the other ~50 wake words, tests, docs. To use another
registry wake word, copy its `model/wakenet_model/<name>` directory from the same
release and select it in menuconfig — `movemodel.py` silently packs nothing for a
model whose directory is missing, so the device then boots with no wake word.

No "Hi Buddy" model exists: a custom wake word is trained by Espressif on request.

Licence: Espressif MIT (`LICENSE`) — free to use on Espressif chips only, which
this device is.
