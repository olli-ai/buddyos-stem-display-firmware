#pragma once
#include "lvgl.h"

/**
 * What the Buddy is doing, on the glass: a small pill at the top of the screen with a
 * coloured dot and a word — waiting for "Jarvis", connecting, listening, thinking,
 * speaking. A child who says the wake word should see the kit answer at once, before
 * omni has said anything.
 *
 * status_set() is called from the voice tasks and only stores the state; an LVGL timer
 * puts it on the screen, so no LVGL call ever runs outside the LVGL task.
 */
typedef enum {
    STATUS_NONE = 0,       /* voice is not running: the pill is hidden */
    STATUS_IDLE,           /* waiting for the wake word */
    STATUS_CONNECTING,     /* wake word heard; the socket is opening */
    STATUS_LISTENING,
    STATUS_THINKING,
    STATUS_SPEAKING,
    STATUS_ERROR,
} status_t;

/** Build the pill on `scr`, above whatever is already there. LVGL task only. */
void status_create(lv_obj_t *scr);

/** Any task. */
void status_set(status_t s);
