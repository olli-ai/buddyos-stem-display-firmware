#pragma once
#include "lvgl.h"
#include "board_pins.h"

/*
 * Text that carries the glass's angle, on a board where BOARD_FACE_ROT_DEG is not 0 —
 * which is the S3 and not the P4.
 *
 * LVGL turns exactly one kind of object without an intermediate layer — lv_image — and
 * a layer is the allocation this firmware is built around not making (the bring-up
 * notes in README are what running out of LVGL's pool looks like). A canvas
 * IS an lv_image, so anything that has to come out level on a tilted panel is
 * rasterised into a canvas in PSRAM, and the canvas is what gets turned.
 *
 * What makes it affordable is when the rasterising happens: once per change of the
 * words, never per frame. The clock redraws a second, the setup screen a step.
 *
 * status.c does the same thing by hand and came first. It is not folded in here
 * because its pill carries a background and a separately pulsing dot, and generalising
 * this far enough to cover that would make it harder to read than two call sites are
 * worth.
 */
#if BOARD_FACE_ROT_DEG != 0

typedef struct {
    const char *text;          /* NULL or "" is skipped, and takes no height */
    const lv_font_t *font;
    uint32_t rgb;
} turned_line_t;

typedef struct {
    lv_obj_t *canvas;
    uint8_t *px;               /* PSRAM, ARGB8888, sized once for the worst case */
    int32_t maxw, maxh;
    int32_t gap;               /* between lines */
    bool said;                 /* whether the overflow warning has been logged */
} turned_t;

/** How wide and tall these lines come out, wrapped at `maxw`. */
void turned_measure(const turned_line_t *lines, int n, int32_t maxw, int32_t gap,
                    int32_t *w, int32_t *h);

/**
 * One canvas, big enough for every block it will ever be asked to hold. False if
 * PSRAM is short, and then every call below is a no-op: a kit with no room degrades
 * to a screen without these words, never to a hang.
 */
bool turned_create(turned_t *t, lv_obj_t *parent, int32_t maxw, int32_t maxh, int32_t gap);

/**
 * Rasterise the lines, centred on each other, and place the block. (ux, uy) is where
 * its centre goes, from the panel's centre, measured BEFORE the glass's angle — the
 * angle is applied here, so callers lay a screen out as if it were level.
 */
void turned_set(turned_t *t, const turned_line_t *lines, int n, int32_t ux, int32_t uy);

/** Place an already-turned block somewhere else without rasterising it again. */
void turned_move(turned_t *t, int32_t ux, int32_t uy);

void turned_show(turned_t *t, bool on);

/** Where (ux, uy) lands once the glass's angle is applied. For callers placing their
 *  own lv_image — the QR on the setup screen — on the same axes. */
void turned_point(int32_t ux, int32_t uy, int32_t *x, int32_t *y);

#endif /* BOARD_FACE_ROT_DEG != 0 */
