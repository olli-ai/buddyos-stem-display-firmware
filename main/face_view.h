#pragma once
#include "lvgl.h"
#include "generated/face_assets.h"
#include "face_catalogue.h"

/** A part id of "artwork" — the face this character's artist drew, which lives in
 *  the catalogue rather than in the baked sprite array. */
#define FACE_PART_ARTWORK (-2)

/** Build the face on `parent` and show `state`. */
void face_view_create(lv_obj_t *parent);
void face_view_set_state(int state);
/** Which state is showing. For callers that did not set it themselves. */
int face_view_state(void);

/* The setters below are safe to call from any task — a directive handler, the voice
 * pipeline — because none of them touch LVGL. They record what is wanted and the
 * next face_view_tick() applies it. Nothing else in this header may be called from
 * another task. */

/** Wear a character from the catalogue, or NULL for the one baked in. */
void face_view_set_character(const face_character_t *c);

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

/** Wear a named expression over whatever the current state draws, until the state
 *  turn ends. `parts` is {browL, eyeL, browR, eyeR, mouth}, or NULL to take it off.
 *  Safe from any task: it records, and the render tick applies it. */
void face_view_request_expression(const int16_t parts[5]);
