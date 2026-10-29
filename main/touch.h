#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * The touch layer, polled over I2C: a GT911 on the Waveshare P4 board, whose bus it
 * shares with the audio codecs. The S3 kit has no touch and does not build this.
 *
 * The controller's part number is not printed anywhere we have, so it is found by what
 * answers: a CST8xx-style chip at 0x15 is what the bench board showed, and the FocalTech
 * and Goodix layouts are kept because the same glass ships with either. Ported from the
 * panel bring-up, where it was seen tracking a finger.
 *
 * Call after the panel is up.
 */

/** Find the controller. ESP_ERR_NOT_FOUND if nothing answers — the gallery runs without. */
esp_err_t touch_init(void);

/** Poll once. True, with x/y in panel pixels, while a finger is down. */
bool touch_read(uint16_t *x, uint16_t *y);
