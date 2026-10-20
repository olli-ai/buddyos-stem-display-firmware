#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * The HCY-CTP00021-A0 touch layer, polled over I2C.
 *
 * The controller's part number is not printed anywhere we have, so it is found by what
 * answers: a CST8xx-style chip at 0x15 is what the bench board showed, and the FocalTech
 * and Goodix layouts are kept because the same glass ships with either. Ported from the
 * panel bring-up, where it was seen tracking a finger.
 *
 * Call after st7701_start(): TP_RST is the panel's reset line, and that is what pulses it.
 */

/** Find the controller. ESP_ERR_NOT_FOUND if nothing answers — the face runs without. */
esp_err_t touch_init(void);

/** Poll once. True, with x/y in panel pixels, while a finger is down. */
bool touch_read(uint16_t *x, uint16_t *y);
