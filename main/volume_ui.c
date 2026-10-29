#include "volume_ui.h"
#include "audio.h"
#include "board_pins.h"
#include "esp_log.h"

static lv_obj_t *s_icon, *s_glyph, *s_panel, *s_track, *s_fill, *s_label;
static bool s_open;
static int s_pending = -1;          /* a show() that arrived before the screen did */

/* A finger is wider than the glyph it is aiming at. */
#define TOUCH_SLOP_PCT 4

static const char *glyph_for(int percent)
{
    if (percent <= 0)  return LV_SYMBOL_MUTE;
    if (percent <= 50) return LV_SYMBOL_VOLUME_MID;
    return LV_SYMBOL_VOLUME_MAX;
}

void volume_ui_create(lv_obj_t *scr)
{
#if LV_FONT_MONTSERRAT_28
    const lv_font_t *font = &lv_font_montserrat_28;
#elif LV_FONT_MONTSERRAT_20
    const lv_font_t *font = &lv_font_montserrat_20;
#else
    const lv_font_t *font = LV_FONT_DEFAULT;
#endif
    const int32_t w = lv_display_get_horizontal_resolution(NULL);
    const int32_t h = lv_display_get_vertical_resolution(NULL);
    const int32_t bar_w = w / 36;
    /* A third of the height, not half: the bar has to fit inside a circle of diameter
     * min(w, h). On 720 the visible half-width at the ends of a half-height bar is
     * only +-311; at a third it is +-339 and the ends clear the bezel. */
    const int32_t bar_h = h / 3;
    const int32_t inset = -w / 16;

    /* ---- the icon: always there, and it never moves ---------------------------
     * Anchored on its own rather than sharing a box with the bar, so opening the bar
     * does not shift the thing you just tapped out from under your finger. */
    s_icon = lv_obj_create(scr);
    lv_obj_remove_style_all(s_icon);
    lv_obj_set_size(s_icon, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_icon, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_icon, LV_OPA_60, 0);
    lv_obj_set_style_radius(s_icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_icon, bar_w / 2, 0);
    lv_obj_remove_flag(s_icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_icon, LV_ALIGN_RIGHT_MID, inset, h / 5);

    s_glyph = lv_label_create(s_icon);
    lv_obj_set_style_text_color(s_glyph, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_glyph, font, 0);
    lv_label_set_text(s_glyph, LV_SYMBOL_VOLUME_MAX);
#if !BOARD_HAS_TOUCH
    /* A control nobody can press is clutter on a 240 px face. The bar below still
     * shows when a volume directive changes the level. */
    lv_obj_add_flag(s_icon, LV_OBJ_FLAG_HIDDEN);
#endif

    /* ---- the bar: above the icon, hidden until asked for ---------------------- */
    s_panel = lv_obj_create(scr);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_panel, bar_w, 0);
    lv_obj_set_style_pad_all(s_panel, bar_w / 2, 0);
    lv_obj_set_style_pad_row(s_panel, bar_w / 2, 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_panel, LV_ALIGN_RIGHT_MID, inset, -h / 16);

    s_label = lv_label_create(s_panel);
    lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_label, font, 0);
    lv_label_set_text(s_label, "0%");

    s_track = lv_obj_create(s_panel);
    lv_obj_remove_style_all(s_track);
    lv_obj_set_size(s_track, bar_w, bar_h);
    lv_obj_set_style_radius(s_track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_track, lv_color_hex(0x4f4f4f), 0);
    lv_obj_set_style_bg_opa(s_track, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_track, LV_OBJ_FLAG_SCROLLABLE);

    s_fill = lv_obj_create(s_track);
    lv_obj_remove_style_all(s_fill);
    lv_obj_set_size(s_fill, bar_w, bar_h);
    lv_obj_set_style_radius(s_fill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_fill, lv_color_hex(0xffb300), 0);
    lv_obj_set_style_bg_opa(s_fill, LV_OPA_COVER, 0);
    /* Grows from the bottom, the way the finger moves. */
    lv_obj_align(s_fill, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);

    volume_ui_show(s_pending >= 0 ? s_pending : audio_get_volume());
    s_pending = -1;
}

bool volume_ui_icon_hit(int x, int y)
{
    if (!s_icon) return false;
    lv_area_t a;
    lv_obj_get_coords(s_icon, &a);
    const int32_t slop = lv_display_get_horizontal_resolution(NULL) * TOUCH_SLOP_PCT / 100;
    return x >= a.x1 - slop && x <= a.x2 + slop && y >= a.y1 - slop && y <= a.y2 + slop;
}

bool volume_ui_is_open(void) { return s_open; }

void volume_ui_toggle(void)
{
    if (!s_panel) return;
    s_open = !s_open;
    if (s_open) {
        volume_ui_show(audio_get_volume());
        lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_panel);
        lv_obj_move_foreground(s_icon);
    } else {
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void volume_ui_show(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (!s_panel) { s_pending = percent; return; }

    lv_label_set_text(s_glyph, glyph_for(percent));

    const int32_t bar_h = lv_obj_get_height(s_track);
    /* Zero height would still draw a circle's worth of fill, so an empty bar is
     * genuinely empty rather than a dot at 0%. */
    int32_t fill = bar_h * percent / 100;
    lv_obj_set_height(s_fill, fill);
    lv_obj_set_style_opa(s_fill, fill ? LV_OPA_COVER : LV_OPA_TRANSP, 0);

    char txt[8];
    lv_snprintf(txt, sizeof(txt), "%d%%", percent);
    lv_label_set_text(s_label, txt);
}
