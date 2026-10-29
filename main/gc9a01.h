#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/**
 * A GC9A01 1.28" 240x240 round TFT on SPI: the ESP32-S3 kit without touch.
 *
 * Ported from the 2026-09-28 bring-up, where this panel first showed the face, not
 * written fresh. The two things it taught are kept: the controller wants its colours
 * inverted, and it clocks RGB565 high byte first — main.c's flush swaps.
 *
 * Unlike the RGB and MIPI panels, a draw is an SPI DMA transfer still running when
 * esp_lcd_panel_draw_bitmap() returns, so the flush is finished from the transfer's
 * completion callback (gc9a01_on_flushed), not inline.
 */

#define GC9A01_W 240
#define GC9A01_H 240

/** Bring up the SPI bus, reset and initialise the panel, and switch it on. `max_px` is
 *  the largest area one flush will send, which sizes the bus's DMA transfers. */
esp_err_t gc9a01_start(esp_lcd_panel_handle_t *out, size_t max_px);

/** Call `cb(arg)` from the SPI driver when a transfer has gone out. ISR context. */
esp_err_t gc9a01_on_flushed(esp_lcd_panel_io_color_trans_done_cb_t cb, void *arg);

/** DISPOFF / DISPON. */
void gc9a01_set_on(bool on);
