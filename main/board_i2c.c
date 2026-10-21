/* The board's shared I2C bus. See board_i2c.h. */
#include "board_i2c.h"

#include "board_pins.h"
#include "esp_log.h"

static i2c_master_bus_handle_t s_bus;

i2c_master_bus_handle_t board_i2c(void)
{
    if (s_bus) return s_bus;
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* both boards have external pull-ups; belt and braces */
    };
    esp_err_t err = i2c_new_master_bus(&bc, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE("i2c", "bus on SDA %d / SCL %d: %s", PIN_I2C_SDA, PIN_I2C_SCL, esp_err_to_name(err));
        s_bus = NULL;
    }
    return s_bus;
}
