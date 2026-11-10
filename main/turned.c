/* Text that carries the glass's angle. See turned.h. */
#include "turned.h"

#include <math.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "turned";

/* sin/cos of the glass's angle, which never changes while the kit is on. */
static float s_sin, s_cos;
static bool s_trig;

static void trig_once(void)
{
    if (s_trig) return;
    const float rad = (float)face_rot_deg() * (float)M_PI / 180.0f;
    s_sin = sinf(rad);
    s_cos = cosf(rad);
    s_trig = true;
}

void turned_point(int32_t ux, int32_t uy, int32_t *x, int32_t *y)
{
    trig_once();
    *x = (int32_t)lroundf((float)ux * s_cos - (float)uy * s_sin);
    *y = (int32_t)lroundf((float)ux * s_sin + (float)uy * s_cos);
}

void turned_measure(const turned_line_t *lines, int n, int32_t maxw, int32_t gap,
                    int32_t *w, int32_t *h)
{
    int32_t ww = 0, hh = 0;
    for (int i = 0; i < n; i++) {
        if (!lines[i].text || !lines[i].text[0]) continue;
        lv_point_t p;
        lv_text_get_size(&p, lines[i].text, lines[i].font, 0, 0, maxw, LV_TEXT_FLAG_NONE);
        if (p.x > ww) ww = p.x;
        if (hh) hh += gap;
        hh += p.y;
    }
    *w = ww;
    *h = hh;
}

bool turned_create(turned_t *t, lv_obj_t *parent, int32_t maxw, int32_t maxh, int32_t gap)
{
    trig_once();
    lv_memzero(t, sizeof(*t));
    t->maxw = maxw;
    t->maxh = maxh;
    t->gap = gap;

    /* Ask LVGL for the stride rather than computing one: lv_canvas_set_buffer will use
     * lv_draw_buf_width_to_stride on every redraw, and a narrower block only ever needs
     * less than this. */
    const size_t need = (size_t)lv_draw_buf_width_to_stride(maxw, LV_COLOR_FORMAT_ARGB8888) * maxh;
    t->px = heap_caps_aligned_alloc(64, (need + 63) & ~(size_t)63,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!t->px) {
        /* Degraded, not blank, and not wrong either: without the buffer there is nothing
         * to rasterise into, so this block simply does not appear. An unturned one over a
         * turned face would be worse — it would look like a bug in the angle. */
        ESP_LOGW(TAG, "no PSRAM for a %dx%d block (%u B); it will not be shown",
                 (int)maxw, (int)maxh, (unsigned)need);
        return false;
    }
    ESP_LOGI(TAG, "%dx%d block, %u B of PSRAM, turned %d deg",
             (int)maxw, (int)maxh, (unsigned)need, face_rot_deg());

    t->canvas = lv_canvas_create(parent);
    lv_obj_remove_flag(t->canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(t->canvas, LV_OBJ_FLAG_HIDDEN);
    return true;
}

void turned_set(turned_t *t, const turned_line_t *lines, int n, int32_t ux, int32_t uy)
{
    if (!t->px) return;

    int32_t w, h;
    turned_measure(lines, n, t->maxw, t->gap, &w, &h);
    if (w <= 0 || h <= 0) { lv_obj_add_flag(t->canvas, LV_OBJ_FLAG_HIDDEN); return; }

    /* Never past what the buffer was sized for. Some of these words come off the
     * network — the SSID the kit is joining is one — and a block a pixel taller than
     * the allocation would be a silent overrun rather than a clipped word. */
    if (w > t->maxw || h > t->maxh) {
        if (!t->said) {
            ESP_LOGW(TAG, "block wanted %dx%d, clipped to %dx%d",
                     (int)w, (int)h, (int)t->maxw, (int)t->maxh);
            t->said = true;
        }
        if (w > t->maxw) w = t->maxw;
        if (h > t->maxh) h = t->maxh;
    }

    lv_canvas_set_buffer(t->canvas, t->px, w, h, LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(t->canvas, lv_color_black(), LV_OPA_TRANSP);

    lv_layer_t layer;
    lv_canvas_init_layer(t->canvas, &layer);

    /* lv_draw_label draws from the top of the area it is given, so the y cursor is the
     * layout: each line gets exactly the height it measured, and the gap follows. */
    int32_t y = 0;
    for (int i = 0; i < n; i++) {
        if (!lines[i].text || !lines[i].text[0]) continue;
        lv_point_t p;
        lv_text_get_size(&p, lines[i].text, lines[i].font, 0, 0, t->maxw, LV_TEXT_FLAG_NONE);

        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.text = lines[i].text;
        d.font = lines[i].font;
        d.color = lv_color_hex(lines[i].rgb);
        d.align = LV_TEXT_ALIGN_CENTER;
        /* The area is the whole block's width, which is the widest line's: wrapping it
         * again at that width gives the same breaks it measured with, because every
         * line it produced already fits inside it. */
        lv_area_t a = { 0, y, w - 1, y + p.y - 1 };
        lv_draw_label(&layer, &d, &a);

        y += p.y + t->gap;
    }

    lv_canvas_finish_layer(t->canvas, &layer);

    lv_image_set_pivot(t->canvas, w / 2, h / 2);
    lv_image_set_rotation(t->canvas, face_rot_deg() * 10);

    lv_obj_remove_flag(t->canvas, LV_OBJ_FLAG_HIDDEN);
    turned_move(t, ux, uy);
}

void turned_move(turned_t *t, int32_t ux, int32_t uy)
{
    if (!t->canvas) return;
    int32_t x, y;
    turned_point(ux, uy, &x, &y);
    lv_obj_align(t->canvas, LV_ALIGN_CENTER, x, y);
}

void turned_show(turned_t *t, bool on)
{
    if (!t->canvas) return;
    if (on) lv_obj_remove_flag(t->canvas, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(t->canvas, LV_OBJ_FLAG_HIDDEN);
}

