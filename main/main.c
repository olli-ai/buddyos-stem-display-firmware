/*
 * BuddyOS STEM — display firmware, photo-gallery build.
 *
 * Photos fill the panel and are swiped through by touch (gallery.c). Behind it the kit
 * is unchanged: BLE Wi-Fi setup, MQTT to the gateway, the "Jarvis" voice conversation
 * and sleep. There is no Buddy face in this build.
 *
 * Two boards, chosen by the IDF target:
 *  - ESP32-S3: a 2.1" 480x480 round ST7701S RGB panel (st7701.c), backlight hardwired.
 *  - ESP32-P4: the Waveshare ESP32-P4-WIFI6-Touch-LCD-4C, a 4" 720x720 JD9365 on
 *    MIPI-DSI (lcd_p4.c), with the radio on the ESP32-C6 beside it.
 *
 * Panel wiring lives in board_pins.h with everything else this firmware drives.
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
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "board_pins.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "driver/gpio.h"
#include "lcd_p4.h"
#define PANEL_NAME      "JD9365"
#define panel_start     lcd_p4_start
#define panel_set_on    lcd_p4_set_on
#else
#include "st7701.h"
#define PANEL_NAME      "ST7701S"
#define panel_start     st7701_start
#define panel_set_on    st7701_set_on
#endif
#include "touch.h"
#if CONFIG_BUDDY_SCREEN_FACE
#include "face_view.h"
#include "face_catalogue.h"
#else
#include "gallery.h"
#endif
#include "status.h"
#include "setup_ui.h"
#include "mem_report.h"
#include "net.h"
#include "sleep.h"
#include "audio.h"

static const char *TAG = "buddy-display";

#define LCD_W  BOARD_LCD_W
#define LCD_H  BOARD_LCD_H

/* A twentieth of the screen, once: on the S3 24 lines, 23 KB of internal RAM — what the
 * two GC9A01 buffers cost before, on a screen with four times the pixels; on the P4 36
 * lines, 52 KB. One buffer, not two: the flush below is a CPU copy into the frame
 * buffer that has finished when it returns, so there is never a transfer in flight for
 * a second buffer to overlap. */
#define BUF_PX  (LCD_W * LCD_H / 20)

static esp_lcd_panel_handle_t s_panel;

/* display.h — the panel's only public switch, so sleep.c need not know about esp_lcd. */
void display_set_on(bool on)
{
    if (s_panel) panel_set_on(on);
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    /* No byte swap. The GC9A01 took RGB565 over SPI high byte first, which is why
     * this used to swap; an RGB panel is fed 16 data lines at once from a uint16_t in
     * memory, so LVGL's host order is exactly what the bus wants.
     *
     * draw_bitmap on an RGB panel copies the area into the PSRAM frame buffer and
     * writes it back from the cache, and has finished when it returns — so the flush
     * is ready here, not in a completion callback. The P4's MIPI-DSI DPI panel does the
     * same (a CPU copy plus a cache write-back, as long as its DMA2D option is off), and
     * so does MIPI's colour order: RGB565 straight from memory, no swap. */
    esp_lcd_panel_draw_bitmap(s_panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
    lv_display_flush_ready(d);
}

/* The touch layer as an LVGL pointer: it is what swipes the gallery. A touch also
 * counts as somebody being there, so a child playing with the screen does not have
 * the kit fall asleep under their finger. */
static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint16_t x, y;
    if (touch_read(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        sleep_note_activity();
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

#if CONFIG_BUDDY_SCREEN_FACE
/* How long an unattached board shows each state before moving on. */
#define STATE_MS 6000

/* The face's own clock. The demo cycle exists only so a board nobody is talking to
 * shows what it can do; the moment a conversation drives the face — or a directive
 * does — it stands aside rather than talking over whoever is in charge. */
static void face_timer(lv_timer_t *t)
{
    (void)t;
    static int64_t origin;
    static int state = -1;
    if (origin == 0) origin = esp_timer_get_time();
    uint32_t ms = (uint32_t)((esp_timer_get_time() - origin) / 1000);
    if (!face_view_is_driven()) {
        int want = (int)((ms / STATE_MS) % (uint32_t)face_state_count);
        if (want != state) { state = want; face_view_set_state(state); }
    } else {
        state = face_view_state();
    }
    face_view_tick(ms);
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "BuddyOS STEM display — " PANEL_NAME " %dx%d", LCD_W, LCD_H);
    mem_report("boot");

#if CONFIG_BUDDY_BOOT_RESET
    /* Before the panel: on the S3 BOOT is GPIO 0, which the panel is about to take as a
     * data line, so this is the only moment it can be read. (The P4's BOOT, GPIO 35,
     * is free all the time; the same moment does for both.) */
    net_boot_reset_check();
#endif

    if (panel_start(&s_panel) != ESP_OK) return;

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    /* Internal RAM, not PSRAM: LVGL reads and writes every pixel of this many times
     * per frame while blending, and PSRAM is the bus the panel is already streaming
     * the frame buffer from. Not DMA — nothing but the CPU touches it. Got before the
     * radio fragments the heap. */
    size_t buf_bytes = BUF_PX * sizeof(uint16_t);
    void *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(TAG, "no internal memory for a %u B draw buffer", (unsigned)buf_bytes); return; }
    lv_display_set_buffers(disp, buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    /* After the panel: on the S3 TP_RST is the panel's reset line, and starting the
     * panel pulsed it. */
    if (touch_init() == ESP_OK) {
        lv_indev_t *tp = lv_indev_create();
        lv_indev_set_type(tp, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(tp, touch_cb);
    }

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
#if CONFIG_BUDDY_SCREEN_FACE
    /* The artwork partition first: a character the child designed lives there, and
     * face_view_create() asks for it while building. Absent, the baked face shows. */
    face_catalogue_mount();
    face_view_create(scr);
    lv_timer_create(face_timer, 16, NULL);          /* ~60 fps */
#else
    gallery_create(scr);
#endif
    status_create(scr);                  /* after the screen: drawn above it */
#if CONFIG_BUDDY_NET
    /* Last of the three, and before net_start() below: a kit that has never been set
     * up calls setup_ui_set() from the net task the moment that task starts. */
    setup_ui_create(scr);
#endif
    ESP_LOGI(TAG, "draw buffer: 1 x %u B internal; frame buffer in PSRAM", (unsigned)buf_bytes);
    mem_report("gallery up");

#if CONFIG_BUDDY_AUDIO
    /* After the panel and before the radio, so a failure here is legible rather than
     * tangled up in Wi-Fi's own logs. Opt-in, and off by default: the first attempt
     * at this took a bench session to untangle from a cable fault and a branch
     * switch, and a device that is otherwise working should not run it every boot. */
    if (audio_start()) {
#if CONFIG_BUDDY_AUDIO_SELFTEST
        audio_selftest();
#endif
    }
#endif

#if CONFIG_BUDDY_NET
    /* Deliberately after the gallery. The draw buffer and the panel's bounce buffers are
     * internal memory and must be got before the radio fragments the heap — the opposite order is
     * how the robokit ended up with BLE unable to start. */
    net_start();
#endif

    /* After the gallery and the radio: sleep_init() reads the wake cause and arms the idle
     * timer, and the timer must not start counting while the panel is still coming up. */
    sleep_init();

    uint32_t report = 0;
    while (1) {
        uint32_t wait = lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(wait > 20 ? 20 : (wait < 2 ? 2 : wait)));
        /* The measurement the planning docs have been arguing about instead of
         * taking. It means little until the radio is up, which is the next step. */
        if (++report > 1500) { report = 0; mem_report("running"); }
    }
}
