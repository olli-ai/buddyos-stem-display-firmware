#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

/**
 * The HD21001C40: a 2.1" 480x480 round ST7701S, configured over bit-banged 3-wire SPI
 * and then fed over a 16-bit RGB bus from a frame buffer in PSRAM.
 *
 * Ported from the panel's bench bring-up (colour bars, gradients and touch, all seen
 * working on this wiring), not written fresh: the init table is the part supplier's
 * sequence for this glass, and every value in it was on screen before it came here.
 */

#define ST7701_W 480
#define ST7701_H 480

/** Reset the panel (and the touch controller, which shares the reset line), send the
 *  init sequence, and start the RGB stream. The frame buffer is black. */
esp_err_t st7701_start(esp_lcd_panel_handle_t *out);

/** DISPOFF + SLPIN, or SLPOUT + DISPON. Borrows the two SPI wires from the RGB bus for
 *  the length of the command; the stream keeps running and the borrowed bits show for
 *  a few hundred microseconds on a screen that is going dark or about to light. */
void st7701_set_on(bool on);
