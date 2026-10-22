/*
 * The whole-head motion presets, on the device, for looking at.
 *
 * Four of the website's presets move the head itself (MOTION_PRESETS in
 * ../buddyos-stem-fe/src/data/motion.ts), and on this panel they do not all fare the
 * same, because the body is composited by the PPA now rather than by LVGL:
 *
 *   nod      face.y  0 -> 3 -> 0           translation  — exact, to the pixel
 *   bounce   face.y -6 -> 0 -> -6          translation  — exact, to the pixel
 *   shake    face.rotation 0 -> -6 -> +6   rotation     — the body cannot: PPA turns
 *                                                         in quarter circles only
 *   breathe  face.scale 1 -> 1.025 -> 1    scale        — the body cannot: PPA
 *                                                         quantises scale to 1/16,
 *                                                         and 1.025 rounds to 1.000
 *
 * So a fifth is added that is not the website's — `shake-x`, the same 700 ms rhythm
 * carried by a horizontal translation instead of a rotation. It is what a head shake
 * can actually be on this hardware, and it is here to be compared against `shake`
 * rather than to replace it.
 *
 * The features keep their own transforms either way: LVGL still scales and rotates
 * brows, eyes and mouth, because they are small. It is only the 518,400-pixel body
 * that is restricted to what the PPA can express.
 *
 * Easing is linear here where the website eases. That is deliberate — this is a
 * measuring stick for the compositor, not a second implementation of sampleMotion(),
 * and face_motion.c already is the faithful one.
 */
#include "sdkconfig.h"

#if CONFIG_BUDDY_FACE_MOTION_DEMO

#include "face_demo_motion.h"
#include <stddef.h>

#define EACH_MS 6000            /* long enough to watch a slow one twice */

typedef struct { uint16_t t; float v; } key_t;

typedef struct {
    const char *name;
    uint16_t    duration;
    uint8_t     prop;           /* 0 x, 1 y, 2 scale, 4 rotation — face_motion's order */
    const key_t *keys;
    uint8_t     n;
} preset_t;

/* nod: kf(0,0), kf(300,3), kf(600,0), kf(1200,0) */
static const key_t NOD[]     = { {0,0}, {300,3}, {600,0}, {1200,0} };
/* bounce: kf(0,-6,easeIn), kf(700,0,bounce), kf(1400,-6,easeOut) */
static const key_t BOUNCE[]  = { {0,-6}, {700,0}, {1400,-6} };
/* shake: kf(0,0), kf(175,-6), kf(350,0), kf(525,6), kf(700,0) */
static const key_t SHAKE[]   = { {0,0}, {175,-6}, {350,0}, {525,6}, {700,0} };
/* breathe: kf(0,1), kf(1500,1.025), kf(3000,1) */
static const key_t BREATHE[] = { {0,1.0f}, {1500,1.025f}, {3000,1.0f} };
/* shake-x: the same rhythm as shake, as a translation this panel can draw. */
static const key_t SHAKE_X[] = { {0,0}, {175,-4}, {350,0}, {525,4}, {700,0} };

static const preset_t PRESETS[] = {
    { "nod (y, exact)",            1200, 1, NOD,     4 },
    { "bounce (y, exact)",         1400, 1, BOUNCE,  3 },
    { "shake-x (x, exact)",         700, 0, SHAKE_X, 5 },
    { "shake (rotation, body still)",700, 4, SHAKE,   5 },
    { "breathe (scale, body still)",3000, 2, BREATHE, 3 },
};
#define N_PRESETS (sizeof PRESETS / sizeof *PRESETS)

static float at(const preset_t *p, uint32_t t)
{
    if (t <= p->keys[0].t) return p->keys[0].v;
    for (uint8_t i = 1; i < p->n; i++) {
        if (t > p->keys[i].t) continue;
        const key_t *a = &p->keys[i - 1], *b = &p->keys[i];
        uint16_t span = b->t - a->t;
        if (!span) return b->v;
        float u = (float)(t - a->t) / span;
        return a->v + (b->v - a->v) * u;
    }
    return p->keys[p->n - 1].v;
}

bool face_demo_motion(uint32_t ms, face_values_t *head, const char **name)
{
    static int last = -1;
    int which = (int)((ms / EACH_MS) % N_PRESETS);
    const preset_t *p = &PRESETS[which];

    float v = at(p, p->duration ? ms % p->duration : 0);
    switch (p->prop) {
        case 0: head->x = v; break;
        case 1: head->y = v; break;
        case 2: head->scale = v; break;
        case 4: head->rotation = v; break;
        default: break;
    }
    if (name) *name = p->name;
    if (which == last) return false;
    last = which;
    return true;
}

#endif /* CONFIG_BUDDY_FACE_MOTION_DEMO */
