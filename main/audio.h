#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"

/**
 * The ears and the mouth.
 *
 * ESP32-P4 (Waveshare 4C board, audio_p4.c): an ES8311 codec into the speaker
 * amplifier and an ES7210 four-channel ADC behind two microphones, on one I2S
 * controller — TDM in, standard out. Same API, same 16 kHz mono, plus the raw slots
 * for echo cancellation below.
 *
 * ESP32-S3 (audio.c): an INMP441 microphone and a MAX98357A amplifier, both I²S.
 *
 * One I²S controller in full duplex: the two parts share a bit clock and a word clock
 * on this board, so capture and playback are the RX and TX halves of the same
 * controller and run at the same time without either waiting. The shared clock forces
 * one slot width on both, 32 bits, which the microphone needs; audio.c does the
 * widening and narrowing so callers still see 16-bit samples.
 *
 * Everything here is 16 kHz mono, which is what the voice path wants end to end:
 * Opus encodes it directly, and the one place a higher rate would help — music — is
 * not what this device does.
 */

#define AUDIO_SAMPLE_RATE 16000

/** Bring up both directions. Safe to call once; returns false if either fails. */
bool audio_start(void);

/** Fill `samples` with mono 16-bit PCM from the microphone. Blocks until full.
 *  Returns how many samples were actually read. */
size_t audio_read(int16_t *samples, size_t count);

/** Play mono 16-bit PCM. Blocks until the buffer is handed to the DMA engine. */
size_t audio_write(const int16_t *samples, size_t count);

/** Power the amplifier. Off between utterances: the MAX98357A draws current and
 *  hisses faintly with nothing to play, which on a device beside a bed is noticed.
 *  A no-op on the current board, whose SD pin is strapped on; see audio.c. */
void audio_speaker_enable(bool on);

/**
 * Prove the wiring, once, at boot.
 *
 * Plays a short tone and reports what the microphone heard, in dBFS. A mic that is
 * not wired reads as digital silence rather than as a quiet room, and the two are
 * easy to tell apart in a log and impossible to tell apart by staring at the board.
 */
void audio_selftest(void);

#if CONFIG_IDF_TARGET_ESP32P4
/** The ES7210's TDM slots, in order. What each carries — microphone, the codec's
 *  playback loopback, or nothing — is CONFIG_BUDDY_AFE_INPUT_FORMAT, and the self-test
 *  prints the evidence for choosing it. */
#define AUDIO_IN_SLOTS 4

/** Fill `frames` frames of AUDIO_IN_SLOTS interleaved 16-bit samples. Blocks until full.
 *  Returns how many frames were read. Use this or audio_read(), not both. */
size_t audio_read_slots(int16_t *frames, size_t count);

/** The software echo reference, for a board with no loopback slot: once enabled, every
 *  sample audio_write() plays is kept, delayed by the TX DMA queue, and handed back
 *  here in step with the microphone — `count` samples, zeros when nothing is playing.
 *  Call from the same task as audio_read_slots(), once per read, with the same count. */
void audio_ref_enable(bool on);
size_t audio_ref_read(int16_t *out, size_t count);
#endif
