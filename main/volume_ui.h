#pragma once

#include "lvgl.h"
#include <stdbool.h>

/*
 * The volume, shown and changed with a finger.
 *
 * A speaker icon sits low on the right and is always there, so the control can be
 * found rather than discovered: tap it and the bar opens, tap it again and it closes.
 * While it is open, dragging up or down anywhere on the glass moves the volume. While
 * it is shut, the glass belongs to the face again and a touch is just a touch.
 *
 * It does not time out. A bar that vanished on its own would be a bar you have to
 * re-open to see where you are, and this one is meant to be read as well as dragged.
 *
 * Both objects live on the active screen, above whatever is drawn there, and neither
 * is clickable: main.c's touch callback owns every press, decides whether it landed on
 * the icon, and calls in here. That way the face keeps seeing the touches it always
 * saw, and LVGL never swallows a drag that started over the bar.
 *
 * Kept off the face on purpose. The first version was a bar across the bottom, which
 * landed on the Buddy's mouth — a design can move a mouth down (face_view.c reads a
 * per-part art_y), so "below the face" is not a place that exists. The right-hand edge
 * is ground the face never claims. The geometry fits inside a circle of diameter
 * min(w, h) rather than reading BOARD_ROUND, so it stays right on a square panel too.
 */

/** Build the icon (shown) and the bar (hidden) on `scr`. Call once, after the screen. */
void volume_ui_create(lv_obj_t *scr);

/** True if (x, y) lands on the icon, with a finger's worth of margin around it. */
bool volume_ui_icon_hit(int x, int y);

/** Open the bar if shut, shut it if open. */
void volume_ui_toggle(void);

/** True while the bar is open — which is when a drag means volume. */
bool volume_ui_is_open(void);

/** Move the bar to `percent`, and pick the icon that matches. */
void volume_ui_show(int percent);
