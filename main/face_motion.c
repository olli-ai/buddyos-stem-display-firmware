/*
 * The `.buddy` interpreter.
 *
 * A deliberate transcription of sampleMotion() in
 * the website's own motion code. Two rules carry over and are the easy ones
 * to get wrong:
 *
 *  - Easing is read from the shipped table, never recomputed from a formula. Two
 *    implementations of easeOutBack in two languages differ in the third decimal
 *    forever and nobody sees it in review.
 *  - A sprite track is stepped. Halfway between part 1 and part 8 is part 4, which is
 *    some other eye entirely.
 */
#include "face_motion.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "motion";

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
static float sample_numeric(const face_key_t *base, const face_track_t *tr, uint32_t t)
{
    const face_key_t *k = &base[tr->first_key];
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

static int16_t sample_sprite(const face_key_t *base, const face_track_t *tr, uint32_t t)
{
    const face_key_t *k = &base[tr->first_key];
    if (tr->n_keys == 0) return -1;
    int16_t cur = k[0].v;
    for (int i = 0; i < tr->n_keys; i++) {
        if (k[i].t > t) break;
        cur = k[i].v;
    }
    return cur;
}

/* ── a design's own motion ───────────────────────────────────────────────────── */

static face_track_t s_rt_tracks[FACE_RT_TRACKS];
static face_key_t   s_rt_keys[FACE_RT_KEYS];
static uint16_t     s_rt_n_tracks, s_rt_n_keys;
/* Per state: where its tracks start, how many, and the loop length it asked for.
 * n_tracks 0 means "this state has no design motion" — fall back to the baked loop,
 * which is how a design that only re-authors `speaking` leaves `idle` alone. */
static struct { uint16_t first, n; uint16_t duration; } s_rt_state[8];

void face_motion_design_clear(void)
{
    s_rt_n_tracks = s_rt_n_keys = 0;
    memset(s_rt_state, 0, sizeof s_rt_state);
}

bool face_motion_has_design(void)
{
    for (unsigned i = 0; i < sizeof s_rt_state / sizeof *s_rt_state; i++)
        if (s_rt_state[i].n) return true;
    return false;
}

bool face_motion_state_has_design(int state)
{
    return state >= 0 && state < (int)(sizeof s_rt_state / sizeof *s_rt_state)
           && s_rt_state[state].n > 0;
}

bool face_motion_design_add(int state, uint16_t duration, uint8_t layer, uint8_t prop,
                            const face_key_t *keys, int n_keys, uint16_t every)
{
    if (state < 0 || state >= (int)(sizeof s_rt_state / sizeof *s_rt_state)) return false;
    if (n_keys <= 0 || layer >= FACE_LAYER_COUNT) return false;
    if (s_rt_n_tracks >= FACE_RT_TRACKS || s_rt_n_keys + n_keys > FACE_RT_KEYS) {
        ESP_LOGW(TAG, "design motion does not fit (%u tracks, %u keys); the rest is dropped",
                 (unsigned)s_rt_n_tracks, (unsigned)s_rt_n_keys);
        return false;
    }
    /* Tracks of one state must be contiguous, so a state can only be filled while it
     * is the one being built. The parser walks states in order, which is the only
     * caller; anything else is a bug and is refused rather than silently interleaved. */
    if (s_rt_state[state].n == 0) s_rt_state[state].first = s_rt_n_tracks;
    else if (s_rt_state[state].first + s_rt_state[state].n != s_rt_n_tracks) return false;

    memcpy(&s_rt_keys[s_rt_n_keys], keys, (size_t)n_keys * sizeof *keys);
    s_rt_tracks[s_rt_n_tracks] = (face_track_t){
        .layer = layer, .prop = prop,
        .first_key = s_rt_n_keys, .n_keys = (uint16_t)n_keys, .every = every,
    };
    s_rt_n_keys += n_keys;
    s_rt_n_tracks++;
    s_rt_state[state].n++;
    s_rt_state[state].duration = duration;
    return true;
}

void face_motion_sample(int state, uint32_t t, face_values_t out[FACE_LAYER_COUNT])
{
    for (int i = 0; i < FACE_LAYER_COUNT; i++) {
        out[i] = (face_values_t){ .x = 0, .y = 0, .scale = 1, .scale_y = 1,
                                  .rotation = 0, .opacity = 1, .sprite = -1 };
    }
    if (state < 0 || state >= face_state_count) return;

    /* A design's motion for this state wins whole, not track by track: mixing the
     * child's tracks with the factory's would have two blink tracks fighting over the
     * same eye. A state the design says nothing about keeps the baked loop. */
    bool design = state < (int)(sizeof s_rt_state / sizeof *s_rt_state)
                  && s_rt_state[state].n > 0;
    const face_track_t *tracks = design ? &s_rt_tracks[s_rt_state[state].first]
                                        : &face_tracks[face_states[state].first_track];
    const face_key_t *keys = design ? s_rt_keys : face_keys;
    int n_tracks = design ? s_rt_state[state].n : face_states[state].n_tracks;
    uint16_t duration = design ? s_rt_state[state].duration : face_states[state].duration;
    if (duration == 0) return;

    uint32_t local = t % duration;
    for (int i = 0; i < n_tracks; i++) {
        const face_track_t *tr = &tracks[i];
        if (tr->layer >= FACE_LAYER_COUNT) continue;
        /* A track with its own period repeats inside the state's loop — which is what
         * lets a 480 ms talking mouth live inside a 4 s blink. */
        uint16_t period = (tr->every && tr->every < duration) ? tr->every : duration;
        uint32_t tt = (period < duration) ? (local % period) : local;
        face_values_t *v = &out[tr->layer];
        if (tr->prop == 255) { v->sprite = sample_sprite(keys, tr, tt); continue; }
        float raw = sample_numeric(keys, tr, tt);
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
