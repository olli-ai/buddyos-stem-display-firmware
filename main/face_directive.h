#pragma once
#include <stdbool.h>
#include <stddef.h>

/**
 * A face designed on the website, arriving over MQTT.
 *
 * What crosses the wire is the child's *choices* — part ids and how the Buddy
 * talks — never pixels. The artwork is already here, baked from the same catalogue
 * the website draws with, so a design is a few hundred bytes and the device does
 * the drawing. That is the whole premise of the faces spec, and it is what makes a
 * redesign feel instant instead of feeling like a download.
 *
 * A part this firmware does not have falls back to what the state already showed,
 * per ADR 0009. That is deliberate and it is not silent: every fallback is logged,
 * because a child who picks star eyes and gets round ones deserves an explanation
 * somewhere.
 */

/** Apply a design. `json` is the directive's `data` object. */
bool face_directive_apply(const char *json, size_t len);

/** Apply it and remember it. This is what the MQTT handler calls. */
bool face_directive_receive(const char *json, size_t len);

/** Re-apply whatever was last applied, if anything. Call once at start-up: a Buddy
 *  that forgets its face when the power blinks is a Buddy that was never designed. */
void face_directive_restore(void);
