#pragma once
#include "lvgl.h"
#include "generated/face_assets.h"

/** Build the face on `parent` and show `state`. */
void face_view_create(lv_obj_t *parent);
void face_view_set_state(int state);
/** Which state is showing. For callers that did not set it themselves. */
int face_view_state(void);
/** Advance the animation. Call from an LVGL timer. */
void face_view_tick(uint32_t elapsed_ms);

/**
 * Let live lip-sync drive the mouth, or -1 to give it back to the animation.
 *
 * Only the mouth: brows, eyes and the head keep the motion the child authored, so a
 * Buddy that blinks and tilts while it speaks goes on doing both. The authored mouth
 * is not lost — releasing puts the designed shape straight back.
 */
void face_view_set_live_mouth(int16_t sprite);
