/*
 * The baked form of a stem-fe design — see tools/bake_face.ts.
 *
 * This is the *in-memory* shape of a face, not the packed `.buddy` wire format of
 * ADR 0010. Translating the website's choices into something a panel can draw is the
 * thing being tested here; the byte-level encoding comes later and rides the same
 * semantics — layers, a closed property set, a stepped sprite track, and easing read
 * from a shipped table rather than recomputed from a formula.
 */
#pragma once
#include <stdint.h>
#include "lvgl.h"

/** One rasterised part, with where it sits in the 240x240 face. */
typedef struct {
    const char *layer;              /* "eyeL", "mouth", ... */
    const char *part;               /* catalogue part id */
    const lv_image_dsc_t *img;      /* NULL when the part draws nothing (brows 'none') */
    int16_t x, y;                   /* top-left of the trimmed sprite */
} face_sprite_t;

/** value is fixed point: x/y 1/16 px, scale & scaleY 1/1024, rotation 1/64 deg,
 *  opacity 0..255, and for a sprite track an index into face_sprites. */
typedef struct { uint16_t t; int16_t v; uint8_t e; } face_key_t;

typedef struct {
    uint8_t  layer;                 /* index into LAYERS: face,browL,eyeL,browR,eyeR,mouth */
    uint8_t  prop;                  /* index into NUMERIC_PROPS, or 255 for a sprite swap */
    uint16_t first_key, n_keys;
    uint16_t every;                 /* this track's own loop period, ms */
} face_track_t;

typedef struct { const char *name; uint16_t duration; uint16_t first_track, n_tracks; } face_state_t;
typedef struct { int16_t sprite; } face_layer_part_t;

#define FACE_LAYER_COUNT 6          /* face, browL, eyeL, browR, eyeR, mouth */
#define FACE_EASING_COUNT 7
#define FACE_LUT_SIZE 65
/* The table stores curve values offset by (y + 1) / 3, because `back` overshoots past
 * 1.0 and a plain 0..65535 row would clip the overshoot flat. Undo with FACE_LUT_VAL. */
#define FACE_LUT_VAL(raw) (((float)(raw) / 65535.0f) * 3.0f - 1.0f)

/* ---- lip-sync ------------------------------------------------------------
 * The mouth shapes are chosen from the audio the Buddy is speaking, never from
 * its text: the product speaks Vietnamese and English and code-switches inside a
 * sentence, and a vowel's formants are the same in either. The thresholds below
 * are generated from the front end's own table so both sides reach the same
 * answer — see ../buddyos-stem-fe/src/data/visemes.ts.
 */
typedef struct {
    int f1lo_a, f1lo_b;   /* Hz. Openness is where the energy sits *inside* F1's */
    int f1hi_a, f1hi_b;   /* range, so it is split rather than measured whole.   */
    int f2_a, f2_b;       /* the second formant: spread and front, or rounded    */
    int sib_a, sib_b;     /* the fricative shelf                                 */
} face_viseme_bands_t;

typedef struct {
    float silence_rms, wide_rms, mbp_rms;   /* fractions of this voice's own peak */
    float teeth_zcr, teeth_flat;            /* a fricative is noise, not brightness */
    float narrow_front, wide_open, mid_open;
    float mbp_low, pucker_low;
    int switch_frames;                      /* frames a new shape must win */
    int min_hold_ms;                        /* ...and the shortest it may show */
} face_viseme_table_t;

/** Viseme order: rest, mbp, small, mid, wide, round, pucker, teeth.
 *  A sprite index, or -1 for `rest`, which means "the mouth the child designed". */
extern const int16_t face_viseme_sprite[8];

/** One way of talking, as the Design page's "How it talks" picker offers it. The
 *  whole table ships because the choice arrives with a design, at runtime. */
typedef struct { const char *id; int16_t sprite[8]; } face_mouth_style_t;
extern const face_mouth_style_t face_mouth_styles[];
extern const int face_mouth_style_count;
extern const face_viseme_bands_t face_viseme_bands;
extern const face_viseme_table_t face_viseme_table;
extern const int face_viseme_frame, face_viseme_hop, face_viseme_rate;

extern const lv_image_dsc_t face_bg;
extern const face_sprite_t  face_sprites[];
extern const int            face_sprite_count;
extern const face_key_t    *face_keys;
extern const face_track_t   face_tracks[];
extern const face_state_t   face_states[];
extern const int            face_state_count;
extern const face_layer_part_t face_design[][5];
extern const uint16_t       face_easing_lut[FACE_EASING_COUNT][FACE_LUT_SIZE];
