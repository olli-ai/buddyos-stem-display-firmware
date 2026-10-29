/* The GC9A01 panel. See gc9a01.h. */
#include "gc9a01.h"

#include "board_pins.h"
#include "driver/spi_master.h"
#include "esp_lcd_gc9a01.h"
#include "esp_log.h"

static const char *TAG = "gc9a01";

/* 40 MHz: what the bring-up ran at. A 240x240 frame is 115 KB, so a full redraw is
 * ~23 ms on the wire; the face redraws a few small sprites per frame, not the screen. */
#define LCD_HZ  (40 * 1000 * 1000)

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;

esp_err_t gc9a01_start(esp_lcd_panel_handle_t *out, size_t max_px)
{
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)(max_px * sizeof(uint16_t)),
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) { ESP_LOGE(TAG, "SPI bus: %s", esp_err_to_name(err)); return err; }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io);
    if (err != ESP_OK) { ESP_LOGE(TAG, "panel IO: %s", esp_err_to_name(err)); return err; }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_gc9a01(s_io, &panel_cfg, &s_panel);
    if (err != ESP_OK) { ESP_LOGE(TAG, "panel: %s", esp_err_to_name(err)); return err; }
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));   /* GC9A01 wants this */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    ESP_LOGI(TAG, "%dx%d on SPI, %d MHz", GC9A01_W, GC9A01_H, LCD_HZ / 1000000);
    *out = s_panel;
    return ESP_OK;
}

esp_err_t gc9a01_on_flushed(esp_lcd_panel_io_color_trans_done_cb_t cb, void *arg)
{
    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = cb };
    return esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, arg);
}

void gc9a01_set_on(bool on)
{
    if (s_panel) esp_lcd_panel_disp_on_off(s_panel, on);
}
