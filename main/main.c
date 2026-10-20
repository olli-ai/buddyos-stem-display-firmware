/*
 * BuddyOS STEM — display firmware, format-translation test.
 *
 * A face designed on the website appears on a 2.1" 480x480 round ST7701S panel and runs
 * the animation the child authored, cycling idle -> listening -> thinking -> speaking.
 *
 * Nothing about the face is written here. The design and its motion are baked from
 * the website's design data by an offline baker; this only drives the panel and hands
 * LVGL its buffers. sim/ renders the identical face_view.c / face_motion.c on a
 * laptop, which is where this was developed, there being no panel to hand.
 *
 * Panel wiring lives in board_pins.h with everything else this firmware drives.
 * The backlight is hardwired on.
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
#include <string.h>
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "st7701.h"
#include "touch.h"
#if CONFIG_BUDDY_GALLERY
#include "gallery.h"
#endif
#include "face_view.h"
#include "face_catalogue.h"
#include "face_lipsync.h"
#include "mem_report.h"
#include "net.h"
#include "sleep.h"
#include "audio.h"

static const char *TAG = "buddy-display";

#define LCD_W  ST7701_W
#define LCD_H  ST7701_H

/* A twentieth of the screen, once: 24 lines, 23 KB of internal RAM — what the two
 * GC9A01 buffers cost before, on a screen with four times the pixels. One buffer, not
 * two: the flush below is a CPU copy into the frame buffer that has finished when it
 * returns, so there is never a transfer in flight for a second buffer to overlap. */
#define BUF_PX  (LCD_W * LCD_H / 20)

/* How long each state is shown. Long enough for a 4 s blink cycle to come round. */
#define STATE_MS 6000

static esp_lcd_panel_handle_t s_panel;

/* display.h — the panel's only public switch, so sleep.c need not know about esp_lcd. */
void display_set_on(bool on)
{
    if (s_panel) st7701_set_on(on);
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    /* No byte swap. The GC9A01 took RGB565 over SPI high byte first, which is why
     * this used to swap; an RGB panel is fed 16 data lines at once from a uint16_t in
     * memory, so LVGL's host order is exactly what the bus wants.
     *
     * draw_bitmap on an RGB panel copies the area into the PSRAM frame buffer and
     * writes it back from the cache, and has finished when it returns — so the flush
     * is ready here, not in a completion callback. */
    esp_lcd_panel_draw_bitmap(s_panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
    lv_display_flush_ready(d);
}

/* The touch layer as an LVGL pointer. Nothing on the face is tappable yet; what a
 * touch does today is count as somebody being there, so a child playing with the
 * screen does not have the kit fall asleep under their finger. */
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

#if CONFIG_BUDDY_LIPSYNC_DEMO
/* 2.4 s of the product's own TTS — Vietnamese then English in one utterance, which
 * is the case the audio-driven design exists for. The same clip tools/xcheck.c holds
 * the classifier to, so what the panel shows is what the website shows. */
extern const uint8_t clip_start[] asm("_binary_lipsync_fixture_pcm_start");
extern const uint8_t clip_end[]   asm("_binary_lipsync_fixture_pcm_end");

/** Play the clip in step with the animation clock while the face is speaking. */
static void lipsync_demo(int state, uint32_t ms)
{
    const int16_t *pcm = (const int16_t *)clip_start;
    const size_t n = (size_t)(clip_end - clip_start) / sizeof(int16_t);
    static size_t at;
    static int was;

    if (strcmp(face_states[state].name, "speaking") != 0) {
        if (was) { face_lipsync_stop(); face_view_set_live_mouth(-1); was = 0; }
        return;
    }
    if (!was) { face_lipsync_start(); at = 0; was = 1; }

    /* State time, not boot time: the clip restarts with each turn at speaking. */
    uint32_t in_state = ms % STATE_MS;
    size_t want = (size_t)((uint64_t)in_state * face_viseme_rate / 1000u);
    if (want > n) want = n;
    if (want < at) { face_lipsync_start(); at = 0; }
    int16_t sprite = -1;
    int before = face_lipsync_frames();
    int64_t t0 = esp_timer_get_time();
    while (at < want) {
        size_t take = want - at < 256 ? want - at : 256;
        sprite = face_lipsync_feed(&pcm[at], take);
        at += take;
    }
    /* What the classifier costs, measured rather than argued about: a 512-point FFT
     * plus a spectral flatness every 16 ms of audio is the one part of this that
     * could plausibly not fit, and nobody has run it on silicon yet. */
    if (face_lipsync_frames() > before) {
        static int64_t total_us;
        static int frames;
        total_us += esp_timer_get_time() - t0;
        if ((frames += face_lipsync_frames() - before) >= 100) {
            ESP_LOGI(TAG, "lip-sync: %lld us per frame (%d ms of audio each)",
                     total_us / frames, face_viseme_hop * 1000 / face_viseme_rate);
            total_us = 0; frames = 0;
        }
    }
    /* Past the end of the clip the Buddy has stopped talking; a mouth left open on
     * its last syllable is the artefact the stop path exists to avoid. */
    if (at >= n) { face_lipsync_stop(); sprite = -1; }
    face_view_set_live_mouth(sprite);
}
#endif

static void face_timer(lv_timer_t *t)
{
    (void)t;
    static int64_t origin;
    static int state = -1;
    if (origin == 0) origin = esp_timer_get_time();
    uint32_t ms = (uint32_t)((esp_timer_get_time() - origin) / 1000);

    /* The demo cycle, which exists only so an unattached board shows what it can
     * do. The moment a conversation drives the face — or a directive does — it
     * stops, rather than talking over whoever is in charge. */
    if (!face_view_is_driven()) {
        int want = (int)((ms / STATE_MS) % (uint32_t)face_state_count);
        if (want != state) { state = want; face_view_set_state(state); }
    } else {
        state = face_view_state();
    }
#if CONFIG_BUDDY_LIPSYNC_DEMO && !CONFIG_BUDDY_VOICE
    lipsync_demo(state, ms);
#endif
    face_view_tick(ms);
}

void app_main(void)
{
    ESP_LOGI(TAG, "BuddyOS STEM display — ST7701S %dx%d", LCD_W, LCD_H);
    mem_report("boot");

#if CONFIG_BUDDY_BOOT_RESET
    /* Before the panel: BOOT is GPIO 0, which the panel is about to take as a data
     * line, so this is the only moment it can be read. */
    net_boot_reset_check();
#endif

    if (st7701_start(&s_panel) != ESP_OK) return;

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

    /* After the panel: TP_RST is the panel's reset line, and st7701_start pulsed it. */
    if (touch_init() == ESP_OK) {
        lv_indev_t *tp = lv_indev_create();
        lv_indev_set_type(tp, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(tp, touch_cb);
    }

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    /* Before the face is built, so the character a design asks for is already
     * available when the stored design is restored a moment later. */
    face_catalogue_mount();
#if CONFIG_BUDDY_GALLERY
    /* The photos have the screen; the face is built hidden behind them, so the voice
     * turn and directives that drive it run unchanged. Without a usable pack this is
     * scr, and the kit is the face alone. */
    face_view_create(gallery_create(scr), CONFIG_BUDDY_FACE_SCALE_PCT);
#else
    face_view_create(scr, CONFIG_BUDDY_FACE_SCALE_PCT);
#endif
    lv_timer_create(face_timer, 16, NULL);          /* ~60 fps */
    ESP_LOGI(TAG, "draw buffer: 1 x %u B internal; frame buffer in PSRAM", (unsigned)buf_bytes);
    mem_report("face up");

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
    /* Deliberately after the face. The draw buffer and the panel's bounce buffers are
     * internal memory and must be got before the radio fragments the heap — the opposite order is
     * how the robokit ended up with BLE unable to start. */
    net_start();
#endif

    /* After the face and the radio: sleep_init() reads the wake cause and arms the idle
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
