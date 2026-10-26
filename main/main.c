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
#include "volume_ui.h"
#include "clock_ui.h"
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
#if CONFIG_IDF_TARGET_ESP32P4
    /* Direct mode: px IS one of the panel's frame buffers, so there is nothing to
     * copy — handing it back to the DPI panel switches which one is scanned out.
     * Only the last call of a frame carries the whole screen, which is the one that
     * should swap; the rest are partial areas of the buffer already being drawn. */
    if (lv_display_flush_is_last(d))
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, px);
#else
    esp_lcd_panel_draw_bitmap(s_panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
#endif
    lv_display_flush_ready(d);
}

/* The touch layer as an LVGL pointer: it is what swipes the gallery. A touch also
 * counts as somebody being there, so a child playing with the screen does not have
 * the kit fall asleep under their finger. */
static bool s_touched;                   /* a finger is down, for edge detection */

/* The volume: tap the icon to open the bar, then drag.
 *
 * The icon is always on the glass, so the control is found rather than discovered —
 * which matters on a kit whose user may not read yet. Tapping it toggles the bar;
 * while the bar is open a drag anywhere means volume, and while it is shut the glass
 * belongs to the face and a touch is just a touch.
 *
 * Gating the drag on the bar being open is what lets both gestures live on one
 * screen. The first version had a drag always mean volume, which worked but took the
 * whole glass away from the face for a control nobody had asked to use yet.
 *
 * DRAG_RANGE_PCT is how much of the screen's height covers the full 0-100: 60% of it,
 * so the range is reachable without lifting off, with room at both ends for a finger
 * that overshoots. DRAG_DEADZONE_PCT keeps a tap a tap.
 *
 * The face's reaction fires on release and only for a tap. It used to fire the moment
 * a finger landed, which reads better for a poke — but it also fired at the start of
 * every volume drag, so the Buddy bounced and giggled while you were turning it down.
 * For a tap the press and release are tens of ms apart and nobody can see it. */
#define DRAG_DEADZONE_PCT 4
#define DRAG_RANGE_PCT    60

static int16_t s_press_y;                /* where the finger landed */
static int s_press_vol;                  /* the volume it landed at */
static bool s_dragging;                  /* past the deadzone: this is a volume gesture */
static bool s_on_icon;                   /* this press began on the volume icon */

static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint16_t x, y;
    if (touch_read(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        sleep_note_activity();
#if CONFIG_BUDDY_CLOCK
        /* The first touch is for dismissing the clock and nothing else: landing on
         * the volume icon behind it would be a press the child could not have aimed. */
        if (clock_ui_visible()) { clock_ui_show(false); data->state = LV_INDEV_STATE_RELEASED; return; }
#endif

        const int32_t h = lv_display_get_vertical_resolution(NULL);
        if (!s_touched) {
            s_touched = true;
            s_press_y = (int16_t)y;
            s_press_vol = audio_get_volume();
            s_dragging = false;
            /* Decided once, on the edge: a finger that slides off the icon mid-press
             * is still the press that opened it, not the start of a drag. */
            s_on_icon = volume_ui_icon_hit(x, y);
            if (s_on_icon) volume_ui_toggle();
        }
        if (!s_on_icon && volume_ui_is_open()) {
            int32_t moved = (int32_t)s_press_y - (int32_t)y;      /* up is louder */
            int32_t dead = h * DRAG_DEADZONE_PCT / 100;
            if (!s_dragging && (moved > dead || -moved > dead)) s_dragging = true;
            if (s_dragging) {
                int want = s_press_vol + (int)(moved * 100 / (h * DRAG_RANGE_PCT / 100));
                if (want < 0) want = 0;
                if (want > 100) want = 100;
                audio_set_volume(want);      /* in memory only; no flash on a drag */
                volume_ui_show(want);
            }
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
#if CONFIG_BUDDY_SCREEN_FACE
        /* A tap on the face, not on the icon and not a drag: the Buddy noticed you. */
        if (s_touched && !s_dragging && !s_on_icon) face_view_touch_react();
#endif
        /* One flash write per gesture, not one per touch frame: see
         * audio_volume_persist(). */
        if (s_dragging) audio_volume_persist();
        s_touched = false;
        s_dragging = false;
        s_on_icon = false;
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
#if CONFIG_BUDDY_CLOCK
    /* Nothing to animate under an opaque clock. Measured on the P4 before this
     * existed: the face went on rendering at ~40 ms a frame, nine times a second,
     * behind a screen showing the time — which is most of a core spent on a picture
     * nobody can see, for as long as the kit is left alone. Stopping here rather
     * than pausing the timer keeps the animation's own clock (`origin`) running, so
     * the demo cycle resumes where it would have been instead of jumping. */
    if (clock_ui_visible()) return;
#endif
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
#if CONFIG_IDF_TARGET_ESP32P4
    /* Draw into the panel's own buffers. Partial rendering meant twenty blend-and-copy
     * passes for a full 720x720 frame and measured 93 ms; here LVGL blends into the
     * memory the display controller is already scanning, and the flush is a buffer
     * swap rather than a megabyte of copying. The S3 keeps the partial path: its
     * RGB panel has no frame buffer to lend, and at 240x240 it did not need one. */
    void *fb0 = NULL, *fb1 = NULL;
    esp_err_t fberr = lcd_p4_buffers(&fb0, &fb1);
    if (fberr != ESP_OK || !fb0 || !fb1) {
        ESP_LOGE(TAG, "no frame buffers from the panel (%s)", esp_err_to_name(fberr));
        return;
    }
    size_t buf_bytes = (size_t)LCD_W * LCD_H * sizeof(uint16_t);
    /* FULL, not DIRECT, and the face is why. The PPA lays the body down under
     * LVGL's features (face_view.c), which means every frame starts as a whole new
     * picture: a partial redraw would leave the previous frame's eyes and mouth
     * standing where the body has just erased everything around them, and the face
     * doubles. Redrawing the whole screen is also cheaper than it sounds here —
     * LVGL still renders only when something has actually changed, and what it
     * renders is a handful of small sprites over a body it never touches. */
    lv_display_set_buffers(disp, fb0, fb1, buf_bytes, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);
    ESP_LOGI(TAG, "draw buffers: 2 x %u B, the panel's own; whole-screen rendering",
             (unsigned)buf_bytes);
#else
    size_t buf_bytes = BUF_PX * sizeof(uint16_t);
    void *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(TAG, "no internal memory for a %u B draw buffer", (unsigned)buf_bytes); return; }
    lv_display_set_buffers(disp, buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
#endif

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
    volume_ui_create(scr);               /* and the volume bar above that */
#if CONFIG_BUDDY_CLOCK
    clock_ui_create(scr);                /* and the clock over everything */
#endif
#if CONFIG_BUDDY_NET
    /* Last of the three, and before net_start() below: a kit that has never been set
     * up calls setup_ui_set() from the net task the moment that task starts. */
    setup_ui_create(scr);
#endif
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
    uint32_t lvgl_frames = 0, lvgl_worst = 0;
    uint64_t lvgl_total = 0;
    while (1) {
        /* What a frame actually costs, which is the only way to tell an optimisation
         * from a change. lv_timer_handler() is where the face is composited and the
         * panel is fed, so its worst case is the frame budget: 16 ms at 60 fps. */
        int64_t t0 = esp_timer_get_time();
        uint32_t wait = lv_timer_handler();
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        if (us > lvgl_worst) lvgl_worst = us;
        lvgl_total += us;
        lvgl_frames++;
        vTaskDelay(pdMS_TO_TICKS(wait > 20 ? 20 : (wait < 2 ? 2 : wait)));
        /* The measurement the planning docs have been arguing about instead of
         * taking. It means little until the radio is up, which is the next step. */
        if (++report > 300) {
            report = 0;
            mem_report("running");
            if (lvgl_frames) {
                ESP_LOGI(TAG, "lvgl: %lu frames, mean %lu us, worst %lu us (16000 is 60 fps)",
                         (unsigned long)lvgl_frames,
                         (unsigned long)(lvgl_total / lvgl_frames),
                         (unsigned long)lvgl_worst);
                lvgl_frames = lvgl_total = lvgl_worst = 0;
            }
        }
    }
}
