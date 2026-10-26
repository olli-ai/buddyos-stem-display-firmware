#pragma once
#include "lvgl.h"

/**
 * The screen: the photos baked by tools/bake_gallery.ps1, one swipe apart, each with
 * its name and a row of page dots.
 *
 * The photos are JPEGs embedded in the app (gallery/gallery.bin), decoded with the
 * TJpgDec LVGL already vendors. Decoded they are 460 KB each, so only the page showing
 * and its two neighbours are kept, in PSRAM — the neighbours so that a finger dragging
 * the next photo into view never finds an empty page.
 *
 * Swiping is a tileview scroll, not a transform: nothing here draws through a layer.
 */

/** Build the gallery on `scr`. With no usable pack it says so on the glass. */
void gallery_create(lv_obj_t *scr);
