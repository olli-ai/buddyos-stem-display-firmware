#pragma once
#include "lvgl.h"
#include "generated/face_assets.h"

/** Build the face on `parent` and show `state`. */
void face_view_create(lv_obj_t *parent);
void face_view_set_state(int state);
/** Advance the animation. Call from an LVGL timer. */
void face_view_tick(uint32_t elapsed_ms);
