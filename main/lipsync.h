#pragma once
#include <stdint.h>
#include <stddef.h>

/**
 * Mouth shapes from the audio the Buddy is speaking — for the gallery's talking photos.
 *
 * The classifier is the face build's, unchanged: a transcription of the website's
 * viseme classifier (../buddyos-stem-fe/src/data/visemes.ts) reading the thresholds the
 * baker copied out of that same table (generated/visemes.c). Two rules carry the weight:
 *
 *  - Loudness is **relative**. Every persona has a cloned voice, so a fixed gate leaves
 *    one Buddy mute and another chewing on room noise.
 *  - A fricative is **noise, not brightness**: zero-crossing rate and spectral flatness,
 *    not energy above 4 kHz.
 *
 * Never from text — the product code-switches inside a sentence — and never from the
 * microphone, which would have the photo mouth what the *child* says. Feed it the PCM
 * on its way to the speaker.
 *
 * What is new is the clock. The PCM is classified when it is handed to the codec, and
 * heard a DMA queue later, so every shape is stamped with when it will be *heard*
 * (CONFIG_BUDDY_LIPSYNC_LEAD_MS ahead) and the screen asks for the shape of "now".
 */

/** Viseme order, as visemes.ts has it. */
enum { LIPSYNC_REST = 0, LIPSYNC_MBP, LIPSYNC_SMALL, LIPSYNC_MID, LIPSYNC_WIDE,
       LIPSYNC_ROUND, LIPSYNC_PUCKER, LIPSYNC_TEETH, LIPSYNC_COUNT };

/** Feed decoded mono PCM at face_viseme_rate (16 kHz), just before it is written to the
 *  codec. Called from the playback task only. */
void lipsync_feed(const int16_t *pcm, size_t samples);

/** Speech ended or was interrupted: the mouth shuts now rather than a queue later. */
void lipsync_stop(void);

/** The shape being heard at this moment, LIPSYNC_REST when nothing is playing. Safe to
 *  call from the LVGL task while the playback task feeds. */
int lipsync_now(void);
