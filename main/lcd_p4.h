#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

/**
 * The Waveshare ESP32-P4-WIFI6-Touch-LCD-4C's panel: a 4" 720x720 JD9365, MIPI-DSI over
 * two lanes, with a frame buffer in PSRAM that the DSI host streams out on its own.
 *
 * The init table and the timings are Waveshare's (their BSP, 720x720 branch); nothing
 * here was tuned by hand.
 */

#define LCD_P4_W 720
#define LCD_P4_H 720

/** Power the DSI PHY, start the bus, reset and initialise the panel, light the
 *  backlight. The frame buffer is black. */
esp_err_t lcd_p4_start(esp_lcd_panel_handle_t *out);

/** Backlight and DISPOFF/DISPON together. */
void lcd_p4_set_on(bool on);
