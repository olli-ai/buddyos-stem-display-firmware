/*
 * The `.buddy` interpreter.
 *
 * A deliberate transcription of sampleMotion() in
 * ../buddyos-stem-fe/src/data/motion.ts. Two rules carry over and are the easy ones
 * to get wrong:
 *
 *  - Easing is read from the shipped table, never recomputed from a formula. Two
 *    implementations of easeOutBack in two languages differ in the third decimal
 *    forever and nobody sees it in review.
 *  - A sprite track is stepped. Halfway between part 1 and part 8 is part 4, which is
 *    some other eye entirely.
 */
#include "face_motion.h"
#include <string.h>

/* Fixed-point divisors, matching the baker. */
#define FP_XY 16.0f
#define FP_SCALE 1024.0f
#define FP_ROT 64.0f

static float ease(uint8_t e, float u)
{
    if (e >= FACE_EASING_COUNT) e = 1;               /* linear */
    const uint16_t *row = face_easing_lut[e];
    if (u <= 0.0f) return FACE_LUT_VAL(row[0]);
    if (u >= 1.0f) return FACE_LUT_VAL(row[FACE_LUT_SIZE - 1]);
    float p = u * (FACE_LUT_SIZE - 1);
    int i = (int)p;
    float f = p - i;
    float a = FACE_LUT_VAL(row[i]), b = FACE_LUT_VAL(row[i + 1]);
    return a + (b - a) * f;
}

/* The easing of a segment comes from the keyframe that *starts* it: the curve says
 * how you leave here for there. Before the first key and after the last, hold. */
static float sample_numeric(const face_track_t *tr, uint32_t t)
{
    const face_key_t *k = &face_keys[tr->first_key];
    if (tr->n_keys == 0) return 0.0f;
    if (t <= k[0].t) return (float)k[0].v;
    const face_key_t *last = &k[tr->n_keys - 1];
    if (t >= last->t) return (float)last->v;
    int i = 0;
    while (i < tr->n_keys - 1 && k[i + 1].t <= t) i++;
    uint16_t span = k[i + 1].t - k[i].t;
    float u = span ? (float)(t - k[i].t) / (float)span : 1.0f;
    return (float)k[i].v + ((float)k[i + 1].v - (float)k[i].v) * ease(k[i].e, u);
}

static int16_t sample_sprite(const face_track_t *tr, uint32_t t)
{
    const face_key_t *k = &face_keys[tr->first_key];
    if (tr->n_keys == 0) return -1;
    int16_t cur = k[0].v;
    for (int i = 0; i < tr->n_keys; i++) {
        if (k[i].t > t) break;
        cur = k[i].v;
    }
    return cur;
}

void face_motion_sample(int state, uint32_t t, face_values_t out[FACE_LAYER_COUNT])
{
    for (int i = 0; i < FACE_LAYER_COUNT; i++) {
        out[i] = (face_values_t){ .x = 0, .y = 0, .scale = 1, .scale_y = 1,
                                  .rotation = 0, .opacity = 1, .sprite = -1 };
    }
    if (state < 0 || state >= face_state_count) return;
    const face_state_t *st = &face_states[state];
    if (st->duration == 0) return;

    uint32_t local = t % st->duration;
    for (int i = 0; i < st->n_tracks; i++) {
        const face_track_t *tr = &face_tracks[st->first_track + i];
        if (tr->layer >= FACE_LAYER_COUNT) continue;
        /* A track with its own period repeats inside the state's loop — which is what
         * lets a 480 ms talking mouth live inside a 4 s blink. */
        uint16_t period = (tr->every && tr->every < st->duration) ? tr->every : st->duration;
        uint32_t tt = (period < st->duration) ? (local % period) : local;
        face_values_t *v = &out[tr->layer];
        if (tr->prop == 255) { v->sprite = sample_sprite(tr, tt); continue; }
        float raw = sample_numeric(tr, tt);
        switch (tr->prop) {
            case 0: v->x = raw / FP_XY; break;
            case 1: v->y = raw / FP_XY; break;
            case 2: v->scale = raw / FP_SCALE; break;
            case 3: v->scale_y = raw / FP_SCALE; break;
            case 4: v->rotation = raw / FP_ROT; break;
            case 5: v->opacity = raw / 255.0f; break;
            default: break;
        }
    }
}
