/*
 * BuddyOS STEM — display firmware, format-translation test.
 *
 * A face designed on the website appears on a GC9A01 round panel and runs the
 * animation the child authored, cycling idle -> listening -> thinking -> speaking.
 *
 * Nothing about the face is written in this file. The design and its motion are
 * baked from ../buddyos-stem-fe by tools/bake_face.ts; this only draws what the
 * translation produced, which is the point of the exercise.
 *
 * Panel wiring (per the newbiely ESP32-S3 round TFT tutorial):
 *   SCL/SCK GPIO11   SDA/MOSI GPIO12   DC GPIO1   CS GPIO13   RST GPIO14
 * The module in that tutorial has no separate backlight pin.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lvgl_port.h"
#include "face_view.h"

static const char *TAG = "buddy-display";

#define PIN_SCLK   11
#define PIN_MOSI   12
#define PIN_DC      1
#define PIN_CS     13
#define PIN_RST    14
#define LCD_W     240
#define LCD_H     240
#define LCD_HZ  (40 * 1000 * 1000)

/* How long each state is shown before moving to the next. Long enough to see a
 * 4 s blink cycle come round at least once. */
#define STATE_MS 6000

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    static int64_t origin;
    static int state;
    if (origin == 0) origin = esp_timer_get_time();
    int64_t ms = (esp_timer_get_time() - origin) / 1000;

    int want = (int)((ms / STATE_MS) % 4);
    if (want != state) { state = want; face_view_set_state(state); }
    face_view_tick((uint32_t)ms);
}

void app_main(void)
{
    ESP_LOGI(TAG, "BuddyOS STEM display — GC9A01 %dx%d", LCD_W, LCD_H);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = LCD_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));   /* GC9A01 wants this */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    lvgl_port_cfg_t lv_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lv_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        /* A tenth of the screen, which is the figure the planning docs budget: a face
         * renderer should cost kilobytes, not a framebuffer. */
        .buffer_size = LCD_W * LCD_H / 10,
        .double_buffer = true,
        .hres = LCD_W,
        .vres = LCD_H,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = true },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    face_view_create(scr);
    lv_timer_create(tick_cb, 16, NULL);          /* ~60 fps */
    lvgl_port_unlock();

    /* What the planning docs actually want measured, reported where it can be read
     * without a debugger: free internal SRAM with the panel up. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "free internal: %u B, largest block: %u B",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
}
