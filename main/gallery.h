#pragma once
#include "lvgl.h"

/**
 * A photo gallery that has the screen: the photos baked by tools/bake_gallery.ps1,
 * one swipe apart. The face is still built, hidden, for what drives it.
 *
 * The photos are JPEGs embedded in the app (gallery/gallery.bin), decoded with the
 * TJpgDec LVGL already vendors. Decoded they are 460 KB each, so only the page showing
 * and its two neighbours are kept, in PSRAM — the neighbours so that a finger dragging
 * the next photo into view never finds an empty page.
 *
 * Swiping is a tileview scroll, not a transform: nothing here draws through a layer.
 */

/** Build the gallery on `scr` and return a hidden holder to create the face on, so
 *  the code that drives the face keeps working with nothing of it on screen.
 *  With no usable pack it returns `scr` itself, and the kit is the face alone. */
lv_obj_t *gallery_create(lv_obj_t *scr);
