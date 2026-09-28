#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * The ears and the mouth: an INMP441 microphone and a MAX98357A amplifier, both I²S.
 *
 * Two separate I²S controllers rather than one shared bus. They run at different
 * times and could share, but a full-duplex call needs capture and playback running
 * at once, and sharing one controller means one of them waits — which is heard as a
 * gap rather than seen as a bug.
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
 *  hisses faintly with nothing to play, which on a device beside a bed is noticed. */
void audio_speaker_enable(bool on);

/**
 * Prove the wiring, once, at boot.
 *
 * Plays a short tone and reports what the microphone heard, in dBFS. A mic that is
 * not wired reads as digital silence rather than as a quiet room, and the two are
 * easy to tell apart in a log and impossible to tell apart by staring at the board.
 */
void audio_selftest(void);
