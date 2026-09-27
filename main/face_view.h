#pragma once
#include "lvgl.h"
#include "generated/face_assets.h"

/** Build the face on `parent` and show `state`. */
void face_view_create(lv_obj_t *parent);
void face_view_set_state(int state);
/** Which state is showing. For callers that did not set it themselves. */
int face_view_state(void);

/** Paint the body. `original` keeps the mascot's own shading; any other colour
 *  fills its silhouette flat, as the website does. */
void face_view_set_body_color(uint32_t rgb, bool original);

/** Show a state, asked for from another task — a directive during a conversation,
 *  and later the voice pipeline itself. Applied on the next render tick. */
void face_view_request_state(int state);

/** True once something outside has taken charge, which stops the demo cycle. */
bool face_view_is_driven(void);

/** Back to the face this firmware was built with — what a deleted design means. */
void face_view_reset_parts(void);

/** Replace one state's five parts — browL, eyeL, browR, eyeR, mouth — with sprites
 *  resolved from a design. Takes effect immediately if that state is on screen. */
void face_view_set_state_parts(int state, const int16_t parts[5]);
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
