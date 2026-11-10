/* The voice status pill. See status.h. */
#include "status.h"
#include "board_pins.h"
#include "face_rot.h"

static volatile status_t s_want = STATUS_NONE;
static status_t s_shown = (status_t)-1;
static lv_obj_t *s_pill, *s_dot;
/* A level pill carries a label; a turned one bakes its caption into the canvas.
 * Both are compiled now, and face_rot_deg() picks between them at startup. */
static lv_obj_t *s_text;

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

/* The pill's measurements, shared by both builds below. */
#define PILL_PAD_H   14    /* ends of the pill, before the dot and after the text */
#define PILL_PAD_V    6
#define PILL_GAP      8    /* dot to text */
#define PILL_TOP     (BOARD_ROUND ? 40 : 20)   /* inside the circle, as the caption is */

/* Opacity is a style, not a transform: the dot is a leaf, and nothing is drawn
 * through a layer. */
static void pulse_cb(void *obj, int32_t v) { lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0); }

/*
 * On a kit whose case turns the glass (face_rot.h), the pill turns with the face, and
 * that is not something LVGL will do to a label or to the flex container below. Any
 * transformed object is drawn through an intermediate layer EXCEPT lv_image — and a
 * layer for even this small a pill is an ARGB8888 allocation out of LVGL's 32 KB pool,
 * which is the allocation this firmware is built around not making.
 *
 * So the pill is drawn into a canvas, and a canvas IS an lv_image: the background, the
 * rounded ends and the glyphs are rasterised once per status change into a buffer in
 * PSRAM, and the widget then carries the glass's angle in the image transform that
 * face_view.c's features already use. The per-draw cost is the same 1,920 B transient
 * that MAX_BUF_SIZE asks for there (4 x 240 x 2, sized off the *display's* RGB565, not
 * off this canvas's ARGB8888), and the measured pool barely moved for it.
 *
 * The dot stays outside the canvas, a leaf of its own placed at the turned position.
 * Two reasons, and the second is the one that matters: a filled circle looks the same
 * at every angle, so it loses nothing by not being in the turned image; and its pulse
 * is an opacity animation, which on a leaf is a style write and inside the canvas would
 * be a full re-rasterisation of the text at animation rate.
 */
#include <math.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "status";

static uint8_t *s_px;              /* the pill's pixels: PSRAM, ARGB8888 */
static const lv_font_t *s_font;
static int32_t s_ph, s_dotsz;      /* height and dot size: the same for every caption */
static int32_t s_dot_fallback_x, s_dot_fallback_y;  /* where the dot sits with no canvas */

/* sin/cos of the glass's angle, which never changes while the kit is on. */
static float s_sin, s_cos;

/* Where a point sits once the glass's angle is applied about the panel's centre. The
 * same mapping face_view.c's place() uses, so the pill lands on the face's axes and
 * not on the panel's. */
static void turn(float ux, float uy, int32_t *x, int32_t *y)
{
    *x = (int32_t)lroundf(ux * s_cos - uy * s_sin);
    *y = (int32_t)lroundf(ux * s_sin + uy * s_cos);
}

/* Rasterise one caption. Called only when the status changes, never per frame. */
static void redraw_pill(status_t s)
{
    lv_point_t t;
    lv_text_get_size(&t, k_look[s].text, s_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

    /* [pad][dot][gap][text][pad] — as wide as this caption needs, like the flex
     * version, which is why the buffer was sized for the longest one. */
    const int32_t w = PILL_PAD_H * 2 + s_dotsz + PILL_GAP + t.x;
    lv_canvas_set_buffer(s_pill, s_px, w, s_ph, LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(s_pill, lv_color_black(), LV_OPA_TRANSP);

    lv_layer_t layer;
    lv_canvas_init_layer(s_pill, &layer);

    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_black();
    bg.bg_opa = LV_OPA_60;
    bg.radius = LV_RADIUS_CIRCLE;
    lv_area_t box = { 0, 0, w - 1, s_ph - 1 };
    lv_draw_rect(&layer, &bg, &box);

    lv_draw_label_dsc_t tx;
    lv_draw_label_dsc_init(&tx);
    tx.text = k_look[s].text;
    tx.font = s_font;
    tx.color = lv_color_white();
    /* lv_draw_label starts at the top of the area, so centre it by hand. */
    const int32_t ty = (s_ph - lv_font_get_line_height(s_font)) / 2;
    lv_area_t tbox = { PILL_PAD_H + s_dotsz + PILL_GAP, ty, w - 1 - PILL_PAD_H, s_ph - 1 };
    lv_draw_label(&layer, &tx, &tbox);

    lv_canvas_finish_layer(s_pill, &layer);

    lv_image_set_pivot(s_pill, w / 2, s_ph / 2);
    lv_image_set_rotation(s_pill, face_rot_deg() * 10);

    /* The pill's centre, unturned, is on the vertical through the panel's centre. */
    int32_t px, py;
    turn(0.0f, (float)(PILL_TOP + s_ph / 2 - BOARD_LCD_H / 2), &px, &py);
    lv_obj_align(s_pill, LV_ALIGN_CENTER, px, py);

    /* And the dot, from the pill's centre, carried round by the same turn. */
    int32_t dx, dy;
    turn((float)(PILL_PAD_H + s_dotsz / 2) - (float)w / 2.0f, 0.0f, &dx, &dy);
    lv_obj_align(s_dot, LV_ALIGN_CENTER, px + dx, py + dy);
}

static void show(bool on)
{
    if (on) {
        /* With no canvas there is nothing in the pill to show, and unhiding an
         * lv_image with no source would leave an empty box over the face. */
        if (!face_rot_deg() || s_px) lv_obj_remove_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_tick(lv_timer_t *t)
{
    (void)t;
    status_t s = s_want;
    if (s == s_shown || (unsigned)s >= sizeof(k_look) / sizeof(k_look[0])) return;
    s_shown = s;
    /* Only the wake-word hint is ever drawn; every other state is left to the face.
     *
     * Two reasons, and the second is the one that was reported. It said what the
     * face already says: there is a designed expression for listening, thinking,
     * speaking and error, and the Buddy is already wearing it, so the word was a
     * caption on a picture of itself. And the pill sits at the top of the glass,
     * which is exactly where a cap, a leaf or a pair of headphones goes — so
     * "Listening" was printed across the accessory the child had chosen to put on
     * their Buddy.
     *
     * An instruction is the one thing a face cannot give. That is what is left. */
    if (s != STATUS_IDLE) { show(false); return; }
    show(true);
    if (face_rot_deg()) { if (s_px) redraw_pill(s); }
    else                 lv_label_set_text(s_text, k_look[s].text);
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

    if (face_rot_deg()) {
    s_font = font;
    s_dotsz = dot;
    s_ph = LV_MAX(dot, (int32_t)lv_font_get_line_height(font)) + PILL_PAD_V * 2;

    const float rad = (float)face_rot_deg() * (float)M_PI / 180.0f;
    s_sin = sinf(rad);
    s_cos = cosf(rad);

    /* One buffer, sized for the only caption that is drawn (see on_tick), so a
     * status change never allocates. */
    int32_t wmax = 0;
    {
        lv_point_t t;
        lv_text_get_size(&t, k_look[STATUS_IDLE].text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        wmax = t.x;
    }
    const int32_t w = PILL_PAD_H * 2 + dot + PILL_GAP + wmax;
    /* Ask LVGL for the stride rather than computing one: lv_canvas_set_buffer will use
     * lv_draw_buf_width_to_stride on every redraw, and a narrower caption only ever
     * needs less than this. */
    const size_t need = (size_t)lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_ARGB8888) * s_ph;

    s_pill = lv_canvas_create(scr);
    s_px = heap_caps_aligned_alloc(64, (need + 63) & ~(size_t)63,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_px) {
        ESP_LOGI(TAG, "pill canvas %dx%d, %u B of PSRAM, turned %d deg",
                 (int)w, (int)s_ph, (unsigned)need, face_rot_deg());
    } else {
        /* Degraded, not blank: without the buffer there is nothing to rasterise into,
         * so the kit runs with no caption rather than with an unturned one over a
         * turned face. The dot is placed by hand below and still says what the Buddy
         * is doing — which colour and whether it pulses is the half a child reads. */
        ESP_LOGW(TAG, "no PSRAM for the pill canvas (%u B); running with the dot only",
                 (unsigned)need);
        int32_t px, py;
        turn(0.0f, (float)(PILL_TOP + s_ph / 2 - BOARD_LCD_H / 2), &px, &py);
        s_dot_fallback_x = px;
        s_dot_fallback_y = py;
    }
    } else {
    s_pill = lv_obj_create(scr);
    lv_obj_remove_style_all(s_pill);
    lv_obj_set_size(s_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_pill, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_pill, LV_OPA_60, 0);
    lv_obj_set_style_radius(s_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(s_pill, PILL_PAD_H, 0);
    lv_obj_set_style_pad_ver(s_pill, PILL_PAD_V, 0);
    lv_obj_set_style_pad_column(s_pill, PILL_GAP, 0);
    lv_obj_set_flex_flow(s_pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_pill, LV_ALIGN_TOP_MID, 0, PILL_TOP);
    }
    /* Taps and swipes go through to the gallery underneath. */
    lv_obj_remove_flag(s_pill, LV_OBJ_FLAG_CLICKABLE);

    /* The dot is a child of the pill where the pill lays itself out, and a sibling on
     * top of it where the pill is a turned image: redraw_pill() places it. */
    s_dot = lv_obj_create(face_rot_deg() ? scr : s_pill);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, dot, dot);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    if (!face_rot_deg()) {
        s_text = lv_label_create(s_pill);
        lv_obj_set_style_text_color(s_text, lv_color_white(), 0);
        lv_obj_set_style_text_font(s_text, font, 0);
    } else if (!s_px) {
        /* redraw_pill() places the dot on every status change, because where it sits
         * depends on how wide that caption made the pill. This is the one case it
         * never runs in. */
        lv_obj_align(s_dot, LV_ALIGN_CENTER, s_dot_fallback_x, s_dot_fallback_y);
    }

    show(false);
    lv_timer_create(on_tick, 50, NULL);
}
