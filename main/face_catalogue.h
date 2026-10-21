#pragma once
#include <stdbool.h>
#include "lvgl.h"

/**
 * The characters, read straight out of the `faces` partition.
 *
 * The app image can afford one mascot's artwork; there are six, and a child who
 * picks Leaf should get Leaf. So the artwork lives in the partition ADR 0009
 * reserves for it — 1,138 KB of 1,536 KB — and is **memory-mapped rather than
 * loaded**: the pixels stay in flash and LVGL draws from them where they lie. It
 * costs no RAM at all, which is the only reason six characters are affordable.
 *
 * It is also replaceable without a firmware release, which is what the partition was
 * for: a new character is a catalogue image, not an OTA for every device.
 */

/** One character: its body, and the face its artist drew on it. */
typedef struct {
    char id[16];
    lv_image_dsc_t bg;
    lv_image_dsc_t art[3];              /* eyeL, eyeR, mouth — w == 0 when absent */
    int16_t art_x[3], art_y[3];
} face_character_t;

/** Map the partition. False means there is none, or it holds something else —
 *  either way the firmware's own baked character still works. */
bool face_catalogue_mount(void);

/** How many characters are in it, and their ids. */
int face_catalogue_count(void);
const char *face_catalogue_id(int i);

/** A character by the id a design names, or NULL. */
const face_character_t *face_catalogue_find(const char *id);
