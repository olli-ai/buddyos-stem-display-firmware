#pragma once

#include "lvgl.h"
#include <stdbool.h>

/*
 * The clock a kit shows when it has been left alone.
 *
 * This is what idle does instead of deep sleep. Blanking the panel made a kit on a
 * shelf look broken, and on the P4 it could only be undone with the RESET button —
 * BOOT is GPIO 35, which cannot wake the chip — so being left alone meant needing an
 * adult. A clock is useful while it waits, and the kit stays awake and listening the
 * whole time, so the wake word still works and a touch still brings the face back.
 *
 * It covers the face rather than replacing it: the face keeps its state underneath
 * and is exactly as it was when the clock goes away.
 *
 * Nothing is shown until SNTP has set the wall clock (net_time_valid). A kit with no
 * network keeps its face, because a clock reading 00:00 is worse than no clock.
 */

/** Build the (hidden) clock on `scr`. Call once, after the screen exists. */
void clock_ui_create(lv_obj_t *scr);

/** Show or hide it. Showing with no network time is a no-op, and says so once. */
void clock_ui_show(bool on);

/** True while the clock is actually on screen. */
bool clock_ui_visible(void);
