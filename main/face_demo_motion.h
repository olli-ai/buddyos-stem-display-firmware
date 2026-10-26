#pragma once
#include "face_motion.h"
#include <stdbool.h>
#include <stdint.h>

/* The website's whole-head motion presets, played one after another on the device,
 * so what the panel can actually do with each one can be seen rather than argued
 * about. Transcribed from MOTION_PRESETS in ../buddyos-stem-fe/src/data/motion.ts.
 *
 * Writes `head` (layer 0) for the preset playing at `ms`, and sets `*name` to its id.
 * Returns true when the preset changed, so the caller can log it. */
bool face_demo_motion(uint32_t ms, face_values_t *head, const char **name);
