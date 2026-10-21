/* The voice status pill. See status.h. */
#include "status.h"

static volatile status_t s_want = STATUS_NONE;
static status_t s_shown = (status_t)-1;
static lv_obj_t *s_pill, *s_dot, *s_text;

void status_set(status_t s) { s_want = s; }

static const struct { const char *text; uint32_t rgb; bool pulse; } k_look[] = {
    [STATUS_NONE]       = { "",            0x000000, false },
    [STATUS_IDLE]       = { "Say \"Jarvis\"", 0x9e9e9e, false },
    [STATUS_CONNECTING] = { "Connecting...", 0xffb300, true  },
    [STATUS_LISTENING]  = { "Listening",   0x43a047, true  },
    [STATUS_THINKING]   = { "Thinking...", 0x1e88e5, true  },
    [STATUS_SPEAKING]   = { "Speaking",    0x8e24aa, false },
    [STATUS_ERROR]      = { "Error",       0xe53935, false },
};

/* Opacity is a style, not a transform: the dot is a leaf, and nothing is drawn
 * through a layer. */
static void pulse_cb(void *obj, int32_t v) { lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0); }

static void on_tick(lv_timer_t *t)
{
    (void)t;
    status_t s = s_want;
    if (s == s_shown || (unsigned)s >= sizeof(k_look) / sizeof(k_look[0])) return;
    s_shown = s;
    if (s == STATUS_NONE) { lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_text, k_look[s].text);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(k_look[s].rgb), 0);
    lv_anim_delete(s_dot, pulse_cb);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    if (k_look[s].pulse) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_dot);
        lv_anim_set_exec_cb(&a, pulse_cb);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_30);
        lv_anim_set_duration(&a, 600);
        lv_anim_set_playback_duration(&a, 600);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
}

void status_create(lv_obj_t *scr)
{
#if LV_FONT_MONTSERRAT_20
    const lv_font_t *font = &lv_font_montserrat_20;
    const int32_t dot = 14;
#else
    const lv_font_t *font = LV_FONT_DEFAULT;
    const int32_t dot = 10;
#endif
    s_pill = lv_obj_create(scr);
    lv_obj_remove_style_all(s_pill);
    lv_obj_set_size(s_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_pill, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_pill, LV_OPA_60, 0);
    lv_obj_set_style_radius(s_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(s_pill, 14, 0);
    lv_obj_set_style_pad_ver(s_pill, 6, 0);
    lv_obj_set_style_pad_column(s_pill, 8, 0);
    lv_obj_set_flex_flow(s_pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* Taps and swipes go through to the gallery underneath. */
    lv_obj_remove_flag(s_pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    /* Inside the circle: the corners of the frame buffer are not on the round glass. */
    lv_obj_align(s_pill, LV_ALIGN_TOP_MID, 0, 40);

    s_dot = lv_obj_create(s_pill);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, dot, dot);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);

    s_text = lv_label_create(s_pill);
    lv_obj_set_style_text_color(s_text, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_text, font, 0);

    lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(on_tick, 50, NULL);
}
