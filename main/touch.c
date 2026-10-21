/* The touch layer. See touch.h. */
#include "touch.h"

#include "board_i2c.h"
#include "board_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

typedef enum {
    TP_NONE,
    TP_CST8XX,   /* CST816 / CST820 / CST826, 0x15 */
    TP_FT6X36,   /* FT6336 / FT5x06, 0x38 */
    TP_GT911,    /* 0x5D or 0x14 */
} tp_type_t;

static i2c_master_dev_handle_t s_dev;
static tp_type_t s_type = TP_NONE;

static esp_err_t rd8(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 50);
}

static esp_err_t rd16(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t r[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_transmit_receive(s_dev, r, 2, buf, len, 50);
}

static esp_err_t wr16(uint16_t reg, uint8_t val)
{
    uint8_t b[3] = { reg >> 8, reg & 0xFF, val };
    return i2c_master_transmit(s_dev, b, 3, 50);
}

esp_err_t touch_init(void)
{
    i2c_master_bus_handle_t bus = board_i2c();
    if (!bus) return ESP_FAIL;

    /* Only the addresses a touch controller answers on. A full scan would do on the S3,
     * where the touch layer is alone on the bus, but on the P4 the codecs share it and
     * the ES8311 at 0x18 would be found first. A CST8xx sleeps within moments of reset
     * and may not ACK a probe, so look a few times before concluding there is nothing. */
    static const uint8_t addrs[] = { 0x15, 0x38, 0x5D, 0x14 };
    uint8_t found = 0;
    for (int attempt = 0; attempt < 5 && !found; attempt++) {
        for (size_t i = 0; i < sizeof(addrs) && !found; i++)
            if (i2c_master_probe(bus, addrs[i], 20) == ESP_OK) found = addrs[i];
        if (!found) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!found) {
        ESP_LOGW(TAG, "nothing on SDA %d / SCL %d — check the touch layer's supply, reset and "
                      "the pull-ups. The gallery runs without touch.", PIN_I2C_SDA, PIN_I2C_SCL);
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = found,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dc, &s_dev);
    if (err != ESP_OK) return err;

    if (found == 0x5D || found == 0x14) {
        uint8_t id[5] = { 0 };
        rd16(0x8140, id, 4);
        ESP_LOGI(TAG, "GT9xx at 0x%02X, product %s", found, (char *)id);
        s_type = TP_GT911;
    } else if (found == 0x38) {
        uint8_t chip = 0;
        rd8(0xA3, &chip, 1);
        ESP_LOGI(TAG, "FocalTech at 0x38, chip 0x%02X", chip);
        s_type = TP_FT6X36;
    } else {
        uint8_t info[3] = { 0 };
        rd8(0xA7, info, 3);                   /* chip id, project id, firmware */
        ESP_LOGI(TAG, "CST8xx-style at 0x%02X, chip 0x%02X proj 0x%02X fw 0x%02X",
                 found, info[0], info[1], info[2]);
        /* Auto-sleep off. Polled with no INT line, a controller that dozes between
         * touches misses the start of every one. */
        uint8_t no_sleep[2] = { 0xFE, 0x01 };
        i2c_master_transmit(s_dev, no_sleep, 2, 50);
        s_type = TP_CST8XX;
    }
    return ESP_OK;
}

bool touch_read(uint16_t *x, uint16_t *y)
{
    uint8_t d[5];

    switch (s_type) {
    case TP_CST8XX:
    case TP_FT6X36:
        /* The same layout from 0x02 on both: count, XH, XL, YH, YL. */
        if (rd8(0x02, d, 5) != ESP_OK) return false;
        /* 0 is no touch; more than 2, or 0xFF, is a busy chip or a glitch on the bus. */
        if ((d[0] & 0x0F) == 0 || (d[0] & 0x0F) > 2) return false;
        *x = ((d[1] & 0x0F) << 8) | d[2];
        *y = ((d[3] & 0x0F) << 8) | d[4];
        /* Never trust the wire: a coordinate off the glass is no touch at all. */
        return *x < BOARD_LCD_W && *y < BOARD_LCD_H;

    case TP_GT911: {
        uint8_t st = 0;
        if (rd16(0x814E, &st, 1) != ESP_OK || !(st & 0x80)) return false;
        bool down = (st & 0x0F) > 0;
        if (down && rd16(0x8150, d, 4) == ESP_OK) {
            *x = d[0] | (d[1] << 8);
            *y = d[2] | (d[3] << 8);
            down = *x < BOARD_LCD_W && *y < BOARD_LCD_H;
        } else {
            down = false;
        }
        wr16(0x814E, 0);
        return down;
    }

    default:
        return false;
    }
}
