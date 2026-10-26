#pragma once
#include "generated/face_assets.h"
#include <stdbool.h>

/** One layer's values at an instant, in the same units the website samples. */
typedef struct {
    float x, y, scale, scale_y, rotation, opacity;
    int16_t sprite;                 /* index into face_sprites, or -1 for "unchanged" */
} face_values_t;

/** Sample every layer of `state` at `t` ms. Mirrors sampleMotion() in
 *  the website's motion implementation — its fixtures are the contract. */
void face_motion_sample(int state, uint32_t t, face_values_t out[FACE_LAYER_COUNT]);

/* ── a design's own motion ────────────────────────────────────────────────────
 *
 * The baked loops are the face this firmware was built with. A child who opens the
 * Design page and picks `shake` is authoring tracks of exactly the same shape, and
 * until now the device logged them and threw them away.
 *
 * These replace the baked tracks for a state, per state: a design that gives
 * `speaking` its own motion leaves `idle` playing what it always did. Capacities are
 * fixed and static — a design arrives from the network and must never be able to
 * decide how much memory this costs.
 */
#define FACE_RT_TRACKS 40
#define FACE_RT_KEYS   320

/** Forget every design track. Called before applying a new design, and when one is
 *  cleared, so the kit goes back to the loops it was built with. */
void face_motion_design_clear(void);

/** Add one track to `state`. `keys` is copied. Values are already in the baked
 *  fixed-point units (x/y x16, scale x1024, rotation x64, opacity 0..255, or a
 *  sprite index), because the parser is the only place that knows how to convert
 *  and the sampler should not learn two encodings.
 *
 *  `every` is the track's own cycle length, 0 for the state's. Returns false when
 *  full, which the caller should log rather than hide: a design that half-applied
 *  looks like a device that half-works. */
bool face_motion_design_add(int state, uint16_t duration, uint8_t layer, uint8_t prop,
                            const face_key_t *keys, int n_keys, uint16_t every);

/** True if any state carries design motion — for the log line, and so a caller can
 *  tell "no motion authored" from "motion authored and dropped". */
bool face_motion_has_design(void);

/** True if `state` plays design tracks rather than the baked loop. */
bool face_motion_state_has_design(int state);
