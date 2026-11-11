# The voice path

The wake word, what leaves the device, and the echo-cancellation work that decides whether a Buddy can be interrupted.

[All docs](README.md) · [Repository README](../README.md)

---

### Talking to it

**Say "Jarvis"** — or **press BTN2 and let go**, on the S3 board, which is the same thing
by the same code path (`CONFIG_BUDDY_TALK_BUTTON`, on by default; the P4 has no second
button). Nothing is sent anywhere until the kit hears the word or feels the press — the microphone feeds ESP-SR WakeNet on the device, and only a detection opens a voice
session. The session closes again after `BUDDY_VOICE_IDLE_S` of quiet.

It is a **stock Espressif WakeNet9 model** (`CONFIG_SR_WN_WN9_JARVIS_TTS`), one of several
that ship with ESP-SR, not something trained here. To use a different one, pick it under
*ESP Speech Recognition* in `idf.py menuconfig` and reflash — the model is packed into the
`model` partition at build time, so changing it changes `srmodels.bin`.

Two known rough edges, both on the detector rather than the pipeline:

- **It may wake itself** if the Buddy's own persona is also called Jarvis and the detector
  is in its aggressive mode (`CONFIG_BUDDY_WAKE_THRESHOLD_MODE=1`).
- **The button is the way in that volume cannot take away.** Above roughly 60% the
  Buddy drowns out its own name, and a press does not care: it raises WakeNet's own
  signal, so idle it opens a conversation and speaking it is a barge-in. It fires on the
  **release** edge, so a held button does not hold the kit open. Armed when the voice
  session starts, which means a kit that never reaches Wi-Fi has a dead button.
- **Half duplex on the S3.** The S3 has one microphone and no echo cancellation, so its
  uplink is muted while the Buddy talks: there you cannot interrupt it just by speaking.
  Say "Jarvis" instead — WakeNet listens to the real microphone *ahead* of that mute, so
  the wake word cuts a reply off mid-sentence. The P4 cancels its own echo and leaves the
  microphone open, so on that board you simply talk over a reply and it stops. See
  [Echo cancellation](voice.md#echo-cancellation-and-the-microphone-slots).

### Echo cancellation and the microphone slots

The ES7210 hands the P4 four TDM slots. `BUDDY_AFE_INPUT_FORMAT` says what each one is,
in ESP-SR's letters — `M` microphone, `R` playback reference, `N` unused — and defaults
to `MRMN`: mic, reference, mic, nothing, as Espressif's P4 boards wire it. If the board
has **no** hardware reference (no `R` in the string), the firmware turns the first `N`
into a software reference: every sample written to the speaker is also queued, aligned
by the DMA depth, and fed to the AFE in that slot. A hardware loopback is better — it
hears the amplifier, not the intention — so prove which one the board has before
trusting either.

`BUDDY_AUDIO_SELFTEST` is how: it prints each slot's level in silence, then plays a
440 Hz tone and prints them again. The slot that rises most is the reference; it warns
if that is not the one the format string calls `R`. Then it shows a live four-slot meter
for ten seconds — talk at each microphone to find which slots they are.

With AEC on, WakeNet listens to the AFE's cleaned output, so "Jarvis" works while the
Buddy is talking, and the microphone stays open during replies. `BUDDY_AEC_KEEP_HALF_DUPLEX`
puts the old behaviour back — mic muted while the Buddy speaks — if the cancellation
turns out not to be enough for the gateway's speech detector.

An open microphone is also what lets the child interrupt **without** the wake word: the
cloud hears them over the reply and stops on its own, then sends a `flush_audio` status.
The kit empties its playback queue on that, because the cloud cannot — half a second of
the abandoned sentence is already here, queued and in the codec's DMA, and playing it
out would sound exactly like not having been heard. Saying "Jarvis" still works and is
still faster, since it needs no round trip. Whether the cloud interrupts itself at all
depends on the persona: `profile_settings.interruption` must not be `none`.

> **The kit needs `BUDDY_UPLINK_GAIN_DB` for any of this to work**, and until
> 2026-10-22 it did not have it. Without the gain the kit was audible to omni's
> recogniser and invisible to its interrupter, which looked like "barge-in is broken"
> and was really "the audio is 40 dB too quiet".
>
> Measured against staging omni's own VAD telemetry, with a working Heymates device on
> the same cluster for comparison:
>
> | | loudness | Silero score | speech episodes |
> | --- | --- | --- | --- |
> | kit, no uplink gain | -62.9 dBFS | 0.03 | none in 114 s |
> | kit, +30 dB | -29.3 dBFS | 0.60-0.83 | start/end, 2.8 s |
> | `HHMST26330000006` | -22.5 dBFS | 0.91-0.97 | normal |
>
> With the gain, barge-in works on the **fast VAD path**, with `vad_processor` loaded
> and left on its defaults — `speaking_started` 30951, `flush_audio` 45219,
> `interrupted` 45268. Talking over the Buddy stops it, as it does in a browser.
>
> Two theories this killed, both of which had been written down as fact. Silero was
> **not** rejecting the kit for being 16 kHz upsampled to 48 — it simply never heard
> speech at -63 dBFS. And `loudness_at_onset` was never the blocker: the editor's
> "Very sensitive" removes that gate and changes nothing, because a 0.03 score fails
> the probability threshold long before any loudness gate is consulted.

### How hard to suppress the echo

`CONFIG_BUDDY_AEC_NLP_LEVEL` — 0 normal, 1 aggressive, 2 very aggressive, **default 2**.
It is a knob rather than a constant because the failures either side of it are both real
on this board, and which one bites depends on the enclosure, the volume and the room.

Too low and the kit hears its own speaker: omni's speech detector takes the Buddy's
voice for the child and the Buddy interrupts itself mid-sentence. Caught in a log at
NORMAL — `speaking_started` 45410, `interrupted` 47819, two and a half seconds into its
own reply.

Measured on the board, echo left in the microphone while the Buddy talks, against a
reference slot at about 15000/32768:

| `BUDDY_AEC_NLP_LEVEL` | residue in the mic |
| --- | --- |
| 1 aggressive | `M:83/833` |
| 2 very aggressive | `M:37/536` |

Turning it to the top roughly halves what leaks through. The cost it is supposed to
carry — NLP suppresses whoever is quieter, and the child is quieter — did not show up:
the wake word was heard 3 of 4 times at aggressive and 5 of 6 at very aggressive. Both
samples are far too small to separate, which is the honest reading; what they do rule
out is the collapse that made this NORMAL in the first place.

**The real fix is level, not suppression.** This board's microphone delivers speech at
about -47 dBFS where `voice.c` reckons arm's length to be -25 — roughly 22 dB quiet.
That is what puts WakeNet near its threshold at *any* NLP setting, what puts the child
below omni's -30 dB VAD gate, and so what forces the trade at all. Note that raising
`CONFIG_BUDDY_MIC_GAIN_DB` is **not** that fix: 30 -> 37 lifted the *reference* slot
about twentyfold rather than the microphones, wrecking the cancellation it was meant to
help. It wants the ES7210's per-channel gain instead.

**What bring-up found** (2026-10-21, one board, P4 silicon v3.2):

- **Ran on the board as written:**
  - the JD9365 panel (ID `93 65 04`);
  - the GT911 at `0x5D`;
  - the C6's hosted firmware over SDIO;
  - BLE setup through the C6;
  - Wi-Fi and MQTT;
  - the AFE pipeline (`AEC → BSS → WakeNet`, slots `MRMN`).
- **The chip revision decides which ESP-SR links.** v3 changed the encoding of the P4's
  vector (PIE) instructions:
  - ESP-SR 2.1.2's P4 libraries were built for older chips. WakeNet's model load faulted
    (`MCAUSE 0x1f` in `dl_esp32p4_memcpy`).
  - The voice session starts once Wi-Fi is up, so the kit rebooted every time it got
    online. From outside, that looked like setup failing.
  - The fix was to move to ESP-SR 2.4.7 (`components/esp-sr/VENDORED.md`).
- **The board has two USB-C ports:**
  - one is the P4's own USB-Serial/JTAG;
  - the other is a CH343 UART.
  - Either one flashes and shows the log, and Windows gives them different COM numbers.
- **"Jarvis" is heard over the Buddy's own voice** and cuts a reply off in ~200 ms. Not yet
  measured: how much of that survives a real, noisy room. With the AFE running, the
  internal heap's largest free block is about 30 KB, so watch it when a session opens.
