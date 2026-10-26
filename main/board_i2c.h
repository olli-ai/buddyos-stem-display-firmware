#pragma once
#include "driver/i2c_master.h"

/**
 * The board's one I2C bus, created on first use.
 *
 * On the S3 bench board only the touch layer is on it. On the Waveshare P4 board the
 * GT911 touch, the ES8311 codec and the ES7210 ADC share SDA 7 / SCL 8, and an I2C port
 * can be opened once: whoever asks first creates it, everyone after gets the same handle.
 * Called from the main task only, during start-up.
 */
i2c_master_bus_handle_t board_i2c(void);
