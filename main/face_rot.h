#pragma once
#include <stdbool.h>

/**
 * How far the glass is turned inside its case, at runtime.
 *
 * This was BOARD_FACE_ROT_DEG, a constant in board_pins.h, because the tilt was a
 * property of one hand-built enclosure. It is not: the kits come out of a 3D printer
 * and the angle belongs to whichever case this board was screwed into, which nobody
 * knows at build time and which changes when the case does. So it is stored on the
 * kit and set from the website, like the speaker volume.
 *
 * BOARD_FACE_ROT_DEG survives as the DEFAULT — what a kit that has never been told
 * otherwise comes up with — so a board whose case is known at build time still works
 * with nothing stored.
 *
 * ## The sign
 *
 * This is the turn applied to the *face*, so it opposes the enclosure's. +30 on a
 * glass already turned 30 clockwise lands at 60 and reads as a much worse tilt than
 * the one being corrected. That mistake has been made on this bench once already
 * (2026-11-03); if a face comes out leaning harder than the bare panel did, the sign
 * is the first thing to look at.
 *
 * ## Why setting it restarts the kit
 *
 * The angle is not a style that can be re-read on the next frame. It is baked into
 * the flattened body pixels (render_rotated), into the pre-rendered head rotations,
 * and into whether the status pill, the clock and the setup screen build themselves
 * as plain widgets or as canvases that can be turned. Every one of those is built
 * once, at startup. Re-laying all of it out live would be a second way to do the same
 * thing, with its own bugs, for a setting changed when a kit is screwed into a case
 * and almost never after. So it is written down and the kit comes back up wearing it.
 */

/** The glass's angle in degrees, -180..180. Always 0 on the P4 — see face_rot.c. */
int face_rot_deg(void);

/**
 * Store an angle for the next boot. Returns false if this board cannot turn its face,
 * or the angle is not a number it can hold.
 *
 * Does NOT change what is on the screen; the caller restarts the kit.
 */
bool face_rot_store(int deg);
