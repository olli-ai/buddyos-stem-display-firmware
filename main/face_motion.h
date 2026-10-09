#pragma once
#include "generated/face_assets.h"

/** One layer's values at an instant, in the same units the website samples. */
typedef struct {
    float x, y, scale, scale_y, rotation, opacity;
    int16_t sprite;                 /* index into face_sprites, or -1 for "unchanged" */
} face_values_t;

/** Sample every layer of `state` at `t` ms. Mirrors sampleMotion() in
 *  the website's motion implementation — its fixtures are the contract. */
void face_motion_sample(int state, uint32_t t, face_values_t out[FACE_LAYER_COUNT]);
