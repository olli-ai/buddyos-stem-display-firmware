/*
 * BuddyOS STEM — display firmware, format-translation test.
 *
 * A face designed on the website appears on a GC9A01 round panel and runs the
 * animation the child authored, cycling idle -> listening -> thinking -> speaking.
 *
 * Nothing about the face is written here. The design and its motion are baked from
 * ../buddyos-stem-fe by tools/bake_face.ts; this only drives the panel and hands
 * LVGL its buffers. sim/ renders the identical face_view.c / face_motion.c on a
 * laptop, which is where this was developed, there being no panel to hand.
 *
 * Panel wiring, from the newbiely ESP32-S3 round TFT tutorial:
 *   SCL/SCK GPIO11   SDA/MOSI GPIO12   DC GPIO1   CS GPIO13   RST GPIO14
 * That module has no separate backlight pin.
 *
 * Dependencies are vendored in components/ and the component manager is switched
 * off (see README): idf 5.2.2's resolver cannot parse a `&&` in a dependency
 * manifest and fails while merely listing registry versions.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "lvgl.h"
#include "face_view.h"
#include "mem_report.h"
#include "net.h"

static const char *TAG = "buddy-display";

#define PIN_SCLK   11
#define PIN_MOSI   12
#define PIN_DC      1
#define PIN_CS     13
#define PIN_RST    14
#define LCD_W     240
#define LCD_H     240
#define LCD_HZ  (40 * 1000 * 1000)

/* A tenth of the screen, twice — the figure the planning docs budget. The simulator
 * runs the same two buffers and reports a ~13 KB LVGL heap peak on top. */
#define BUF_PX  (LCD_W * LCD_H / 10)

/* How long each state is shown. Long enough for a 4 s blink cycle to come round. */
#define STATE_MS 6000

static esp_lcd_panel_handle_t s_panel;

/* The panel transfer is asynchronous, so LVGL is told the flush is done from the
 * driver's completion callback rather than when draw_bitmap returns. Getting this
 * wrong means LVGL reuses a buffer the DMA engine is still reading. */
static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *arg)
{
    (void)io; (void)e;
    lv_display_flush_ready((lv_display_t *)arg);
    return false;
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    esp_lcd_panel_draw_bitmap(s_panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
    (void)d;
}

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void face_timer(lv_timer_t *t)
{
    (void)t;
    static int64_t origin;
    static int state = -1;
    if (origin == 0) origin = esp_timer_get_time();
    uint32_t ms = (uint32_t)((esp_timer_get_time() - origin) / 1000);

    int want = (int)((ms / STATE_MS) % (uint32_t)face_state_count);
    if (want != state) { state = want; face_view_set_state(state); }
    face_view_tick(ms);
}

void app_main(void)
{
    ESP_LOGI(TAG, "BuddyOS STEM display — GC9A01 %dx%d", LCD_W, LCD_H);
    mem_report("boot");

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BUF_PX * (int)sizeof(uint16_t),
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

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));   /* GC9A01 wants this */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    /* DMA-capable internal memory: PSRAM cannot feed the SPI engine directly, and
     * this is the allocation that will bite if the radio has taken the heap first. */
    size_t buf_bytes = BUF_PX * sizeof(uint16_t);
    void *b1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *b2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!b1 || !b2) { ESP_LOGE(TAG, "no DMA memory for %u B draw buffers", (unsigned)buf_bytes); return; }
    lv_display_set_buffers(disp, b1, b2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_trans_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &cbs, disp));

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    face_view_create(scr);
    lv_timer_create(face_timer, 16, NULL);          /* ~60 fps */
    ESP_LOGI(TAG, "draw buffers: 2 x %u B DMA", (unsigned)buf_bytes);
    mem_report("face up");

#if CONFIG_BUDDY_NET
    /* Deliberately after the face. The panel buffers are DMA-capable internal memory
     * and must be got before the radio fragments the heap — the opposite order is
     * how the robokit ended up with BLE unable to start. */
    net_start();
#endif

    uint32_t report = 0;
    while (1) {
        uint32_t wait = lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(wait > 20 ? 20 : (wait < 2 ? 2 : wait)));
        /* The measurement the planning docs have been arguing about instead of
         * taking. It means little until the radio is up, which is the next step. */
        if (++report > 1500) { report = 0; mem_report("running"); }
    }
}
