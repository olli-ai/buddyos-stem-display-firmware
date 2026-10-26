#include "clock_ui.h"
#include "board_pins.h"
#include "net.h"
#include "sleep.h"
#include "voice.h"
#include "esp_log.h"
#include <time.h>

static const char *TAG = "clock";

static lv_obj_t *s_box, *s_time, *s_date;
static lv_timer_t *s_tick;
static bool s_on;

/* One timer asks both questions once a second: should the clock be up, and what time
 * is it. It runs inside LVGL's own loop, which is the only place LVGL may be touched
 * from — this firmware has no lock around it, and crossing that line from a task of
 * its own is what face_view_request_state() exists to avoid. Everything this reads
 * (sleep_idle_ms, voice_active, time) is a plain load from somewhere else. */
#define TICK_MS 1000

static void draw_now(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    char hhmmss[16], date[48];
    strftime(hhmmss, sizeof(hhmmss), "%H:%M:%S", &tm);
    /* Weekday and date, in the kit's own words rather than a locale's: the firmware
     * carries no locale data, and strftime's %A would answer in C-locale English on
     * every board regardless of the child's language. */
    static const char *DAYS[7] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                   "Thursday", "Friday", "Saturday" };
    static const char *MONTHS[12] = { "January", "February", "March", "April", "May",
                                      "June", "July", "August", "September", "October",
                                      "November", "December" };
    lv_snprintf(date, sizeof(date), "%s, %d %s %d",
                DAYS[tm.tm_wday % 7], tm.tm_mday, MONTHS[tm.tm_mon % 12], 1900 + tm.tm_year);

    lv_label_set_text(s_time, hhmmss);
    lv_label_set_text(s_date, date);
}

static void on_tick(lv_timer_t *t)
{
    (void)t;
    /* A conversation counts as company even while nobody is saying anything, so the
     * clock cannot arrive over a reply that is still being spoken. */
    bool want = !voice_active() &&
                sleep_idle_ms() >= (int64_t)CONFIG_BUDDY_CLOCK_IDLE_S * 1000;
    if (want != s_on) clock_ui_show(want);
    if (s_on) draw_now();
}

void clock_ui_create(lv_obj_t *scr)
{
    const int32_t w = lv_display_get_horizontal_resolution(NULL);

    /* Opaque and full-screen: it covers the face rather than replacing it, so the
     * face keeps its state and comes back exactly as it was. On the P4 the body is
     * composited by the PPA underneath LVGL, so a transparent panel here would show
     * the Buddy through the digits. */
    s_box = lv_obj_create(scr);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_box, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_box, w / 40, 0);
    /* Takes no touches: main.c's indev callback owns every press, and the first one
     * puts the face back. An overlay that swallowed it would need two taps. */
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_time = lv_label_create(s_box);
    lv_obj_set_style_text_color(s_time, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_48
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_48, 0);
#elif LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_28, 0);
#endif
    lv_label_set_text(s_time, "--:--:--");

    s_date = lv_label_create(s_box);
    lv_obj_set_style_text_color(s_date, lv_color_hex(0x9e9e9e), 0);
#if LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(s_date, &lv_font_montserrat_28, 0);
#elif LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(s_date, &lv_font_montserrat_20, 0);
#endif
    lv_label_set_text(s_date, "");

    lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    /* Never paused: it is the thing that decides the clock should come back. */
    s_tick = lv_timer_create(on_tick, TICK_MS, NULL);
}

bool clock_ui_visible(void) { return s_on; }

void clock_ui_show(bool on)
{
    if (!s_box || on == s_on) return;

    if (on && !net_time_valid()) {
        static bool said;
        if (!said) {
            said = true;
            ESP_LOGI(TAG, "no network time yet; keeping the face instead of a clock");
        }
        return;
    }
    s_on = on;
    if (on) {
        draw_now();                      /* before it is seen, not a second after */
        lv_obj_remove_flag(s_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_box);
        ESP_LOGI(TAG, "left alone; showing the time");
    } else {
        lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "back to the face");
    }
}
