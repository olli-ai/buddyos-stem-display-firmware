#pragma once
#include <stdint.h>
#include <stddef.h>

/**
 * Mouth shapes from the audio the Buddy is speaking.
 *
 * Never from its text: this product speaks Vietnamese and English and switches
 * between them inside a sentence, and text would mean grapheme-to-phoneme rules per
 * language. A vowel's formants are the same in either, and a fricative is noise in
 * both — so the classifier hears, and needs to know no language at all.
 *
 * It is a transcription of the website's viseme classifier, reading the
 * thresholds the baker copied out of that same table. Two rules carry the weight:
 *
 *  - Loudness is **relative**. Every persona has a cloned voice, so a fixed gate
 *    leaves one Buddy mute and another chewing on room noise. Thresholds are
 *    fractions of what this voice has recently been doing.
 *  - A fricative is **noise, not brightness**. Measuring energy above 4 kHz says how
 *    bright a voice is, not what it is saying; a breathy voice trips it on half its
 *    vowels. Zero-crossing rate and spectral flatness ask whether the frame is
 *    noise-shaped instead.
 *
 * Feed it the PCM on its way to the speaker, never the microphone: a device with
 * both would otherwise lip-sync to the room, and to itself.
 */

/** Start of an utterance. Clears the loudness reference and shuts the mouth. */
void face_lipsync_start(void);

/**
 * Feed decoded mono PCM at `face_viseme_rate`.
 *
 * Returns the sprite index to draw, or -1 for "the mouth the child designed", which
 * is what a pause should look like. Time is counted in samples, not from the system
 * clock, so a late buffer does not shorten a shape and the whole thing reproduces
 * off-device — which is how tools/xcheck.c holds it to the front end's answer.
 */
int16_t face_lipsync_feed(const int16_t *pcm, size_t samples);

/** Which mouth each of the eight classes draws, replacing the baked default. This
 *  is the child's "How it talks" choice arriving from the website. */
void face_lipsync_set_mouths(const int16_t sprite[8]);

/** The class currently shown, 0..7 in VISEMES order. For the conformance check. */
int face_lipsync_viseme(void);
/** Frames classified since the last start(). */
int face_lipsync_frames(void);

/** Speech ended, or the child interrupted. Shuts the mouth on this frame. */
void face_lipsync_stop(void);
