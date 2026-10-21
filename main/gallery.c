/* Photo gallery. See gallery.h for the shape, tools/bake_gallery.ps1 for the pack. */
#include "gallery.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board_pins.h"
#include "lipsync.h"
#include "mem_report.h"

#include "src/libs/tjpgd/tjpgd.h"

static const char *TAG = "gallery";

#define MAX_PHOTOS 64
#define NAME_LEN   24
#define MAX_SIDE   BOARD_LCD_W         /* the panel; the baker never writes more */

/* The pack, little-endian like everything else here:
 *   "GLRY"  u32 count
 *   count x { u32 offset; u32 size; char name[24]; }   offsets from the pack's start
 *   the JPEGs
 * or "GLR2", whose entries also say where the mouth is, in pixels of the baked photo:
 *   count x { u32 offset; u32 size; char name[24]; u16 mx, my, mw, jaw; }
 * (mx, my) is the middle of the lips' seam, mw the mouth's width, jaw the seam to the
 * chin; all zero for a photo that does not talk. Embedded rather than downloaded, but
 * read as if it were not: a stale or truncated pack degrades to fewer photos, or to a
 * photo that keeps its mouth still, never to a fault. */
#if CONFIG_IDF_TARGET_ESP32P4
extern const uint8_t pack_start[] asm("_binary_gallery_720_bin_start");
extern const uint8_t pack_end[]   asm("_binary_gallery_720_bin_end");
#else
extern const uint8_t pack_start[] asm("_binary_gallery_bin_start");
extern const uint8_t pack_end[]   asm("_binary_gallery_bin_end");
#endif

typedef struct {
    const uint8_t *jpg;
    uint32_t size;
    char name[NAME_LEN];
    lv_obj_t *img;
    lv_image_dsc_t dsc;                /* .data is NULL while not decoded */
    /* Talking: where the mouth is, in pixels of this photo, as the pack says. */
    uint16_t mx, my, mw, jaw;
    lv_obj_t *hole, *chin;             /* children of img: the mouth's inside, the jaw */
    lv_image_dsc_t chin_dsc;           /* ARGB8888, cut out of dsc when it is decoded */
    int16_t chin_x, chin_y;            /* where the jaw sits when the mouth is shut */
    float open, width;                 /* eased toward the shape being heard */
    int16_t shown_drop, shown_w;
} photo_t;

/* PSRAM: 3.8 KB that is read once per swipe, and internal SRAM is what voice is short of. */
EXT_RAM_BSS_ATTR static photo_t s_photo[MAX_PHOTOS];
static int s_count, s_page = -1;
static lv_obj_t *s_tv, *s_caption, *s_dots;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int load_pack(void)
{
    size_t len = (size_t)(pack_end - pack_start);
    bool v2 = len >= 8 && memcmp(pack_start, "GLR2", 4) == 0;
    if (len < 8 || (!v2 && memcmp(pack_start, "GLRY", 4) != 0)) {
        ESP_LOGW(TAG, "no gallery pack (run tools/bake_gallery.ps1)");
        return 0;
    }
    const size_t esz = 8 + NAME_LEN + (v2 ? 8 : 0);
    uint32_t n = rd32(pack_start + 4);
    if (n > MAX_PHOTOS) n = MAX_PHOTOS;
    int kept = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = pack_start + 8 + i * esz;
        if (e + esz > pack_end) break;
        uint32_t off = rd32(e), size = rd32(e + 4);
        if (off > len || size > len - off || size < 4) {
            ESP_LOGW(TAG, "photo %u lies outside the pack; skipped", (unsigned)i);
            continue;
        }
        photo_t *p = &s_photo[kept++];
        p->jpg = pack_start + off;
        p->size = size;
        memcpy(p->name, e + 8, NAME_LEN);
        p->name[NAME_LEN - 1] = '\0';
        /* Checked against the photo's real size when it is decoded: see chin_cut(). */
        const uint8_t *m = e + 8 + NAME_LEN;
        p->mx = v2 ? rd16(m) : 0;
        p->my = v2 ? rd16(m + 2) : 0;
        p->mw = v2 ? rd16(m + 4) : 0;
        p->jaw = v2 ? rd16(m + 6) : 0;
    }
    return kept;
}

/* ---- decoding ------------------------------------------------------------- */

typedef struct {
    const uint8_t *src;
    size_t len, at;
    uint16_t *dst;
    uint16_t w;
} jctx_t;

static size_t jd_in(JDEC *jd, uint8_t *buf, size_t n)
{
    jctx_t *c = jd->device;
    if (n > c->len - c->at) n = c->len - c->at;
    if (buf) memcpy(buf, c->src + c->at, n);
    c->at += n;
    return n;
}

/* LVGL's TJpgDec is configured for 24-bit output in B, G, R order. */
static int jd_out(JDEC *jd, void *bitmap, JRECT *r)
{
    jctx_t *c = jd->device;
    const uint8_t *px = bitmap;
    for (unsigned y = r->top; y <= r->bottom; y++) {
        uint16_t *d = c->dst + (size_t)y * c->w + r->left;
        for (unsigned x = r->left; x <= r->right; x++, px += 3)
            *d++ = (uint16_t)((px[2] >> 3) << 11 | (px[1] >> 2) << 5 | px[0] >> 3);
    }
    return 1;
}

/* ---- talking -------------------------------------------------------------- */

/* A photo talks the way a puppet does: its jaw drops. The jaw is a sprite cut out of the
 * photo itself — from the lips' seam down past the chin, opaque in the middle and fading
 * to nothing at its edge — laid exactly over where it came from, so a shut mouth is the
 * photo untouched. Opening slides it down and shows what is under it: a dark, rounded
 * hole with a little light at the top for teeth. Two plain children of the image, moved
 * and resized; no transform anywhere (CLAUDE.md: a transformed container is drawn
 * through a layer the size of the screen). */

#define JAW_W_NUM   12     /* the jaw sprite is 2.4 mouths wide: cheek to cheek */
#define JAW_W_DEN   5
#define JAW_DROP    4      /* wide open drops the jaw by a quarter of seam-to-chin */

/* Opening and width, in percent, for each viseme. The website draws a cartoon mouth
 * from the same eight shapes; a photo can only drop its jaw and narrow its hole, so
 * this is the part of each shape a jaw can say. */
static const uint8_t k_shape[LIPSYNC_COUNT][2] = {
    [LIPSYNC_REST]   = {   0, 100 },
    [LIPSYNC_MBP]    = {   0, 100 },
    [LIPSYNC_SMALL]  = {  35,  80 },
    [LIPSYNC_MID]    = {  60,  90 },
    [LIPSYNC_WIDE]   = { 100, 100 },
    [LIPSYNC_ROUND]  = {  55,  65 },
    [LIPSYNC_PUCKER] = {  30,  55 },
    [LIPSYNC_TEETH]  = {  20, 100 },
};

static float smooth(float e0, float e1, float x)
{
    float t = (x - e0) / (e1 - e0);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

/* Cut the jaw out of a freshly decoded photo. The pack's mouth is checked against the
 * photo's real size here: a mouth that does not fit leaves the photo still, not broken. */
static void chin_cut(photo_t *p)
{
    const int w = p->dsc.header.w, h = p->dsc.header.h;
    if (!p->chin || p->mw < 8 || p->jaw < 8 || p->mx >= w || p->my >= h) return;

    int jw = p->mw * JAW_W_NUM / JAW_W_DEN;
    if (jw > w) jw = w;
    int x0 = p->mx - jw / 2;
    if (x0 < 0) x0 = 0;
    if (x0 + jw > w) x0 = w - jw;
    /* The jaw starts a few rows under the seam: the seam is the darkest line on a face,
     * and carried down with the jaw it would show twice beside the opening. */
    const int top = p->my + (p->jaw / 25 > 2 ? p->jaw / 25 : 2);
    int jh = p->jaw + p->jaw / 6;                  /* a little below the chin, to fade in */
    if (jh > h - top) jh = h - top;
    if (jh < 8) return;

    uint8_t *px = heap_caps_malloc((size_t)jw * jh * 4, MALLOC_CAP_SPIRAM);
    if (!px) { ESP_LOGW(TAG, "%s: no PSRAM for the jaw; it stays still", p->name); return; }

    const uint16_t *src = (const uint16_t *)p->dsc.data;
    /* Opaque in an ellipse under the mouth, fading out at its rim. Along the seam it is
     * opaque only as wide as the mouth: the corners of a mouth stay where they are when
     * it opens, so the cheeks beside them stretch rather than tear along a straight
     * line. A shut mouth hides all of this, since the sprite lies on its own pixels. */
    const float cx = p->mx - x0, rx = jw / 2.0f, ry = (float)p->jaw, rm = p->mw / 2.0f;
    uint8_t *d = px;
    for (int y = 0; y < jh; y++) {
        const uint16_t *s = src + (size_t)(top + y) * w + x0;
        float dy = (top - p->my + y) / ry;
        float low = smooth(0.0f, 0.45f, dy);           /* 0 at the seam, 1 below the lip */
        for (int x = 0; x < jw; x++, d += 4) {
            uint16_t v = s[x];
            float dx = (x - cx) / rx;
            float corner = 1.0f - smooth(0.6f, 1.15f, fabsf(x - cx) / rm);
            float a = (1.0f - smooth(0.6f, 1.0f, sqrtf(dx * dx + dy * dy)))
                    * (corner + (1.0f - corner) * low);
            d[0] = (uint8_t)((v & 0x1f) << 3 | (v & 0x1f) >> 2);          /* B */
            d[1] = (uint8_t)((v >> 5 & 0x3f) << 2 | (v >> 5 & 0x3f) >> 4); /* G */
            d[2] = (uint8_t)((v >> 11) << 3 | (v >> 11) >> 2);             /* R */
            d[3] = (uint8_t)(a * 255.0f + 0.5f);
        }
    }
    p->chin_dsc = (lv_image_dsc_t){
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888,
                    .w = jw, .h = jh, .stride = jw * 4 },
        .data_size = (uint32_t)jw * jh * 4,
        .data = px,
    };
    p->chin_x = x0;
    p->chin_y = (int16_t)top;
    p->open = 0;
    p->width = 1;
    p->shown_drop = -1;                            /* forces the first placement */
    lv_image_set_src(p->chin, &p->chin_dsc);
    lv_obj_set_pos(p->chin, x0, top);
    lv_obj_remove_flag(p->chin, LV_OBJ_FLAG_HIDDEN);
}

/* Put the jaw and the hole where `open` and `width` say. Only touches LVGL on a change of
 * a whole pixel, so a closed mouth costs nothing to redraw. */
static void mouth_place(photo_t *p)
{
    int drop = (int)(p->open * p->jaw / JAW_DROP + 0.5f);
    int hw = (int)(p->width * p->mw * 0.85f + 0.5f);   /* the corners do not open */
    if (drop == p->shown_drop && hw == p->shown_w) return;
    p->shown_drop = (int16_t)drop;
    p->shown_w = (int16_t)hw;

    lv_obj_set_pos(p->chin, p->chin_x, p->chin_y + drop);
    if (drop < 1) {
        lv_obj_add_flag(p->hole, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_size(p->hole, hw, p->chin_y - p->my + drop + 5);
    lv_obj_set_pos(p->hole, p->mx - hw / 2, p->my - 1);
    lv_obj_remove_flag(p->hole, LV_OBJ_FLAG_HIDDEN);
}

#if CONFIG_BUDDY_LIPSYNC_DEMO
/* Talking with nothing to say: a fixed babble, so the jaw can be judged on the bench
 * without a voice session. 130 ms a shape, then a pause. */
static int demo_viseme(void)
{
    static const uint8_t seq[] = {
        LIPSYNC_WIDE, LIPSYNC_SMALL, LIPSYNC_MBP, LIPSYNC_MID, LIPSYNC_ROUND, LIPSYNC_TEETH,
        LIPSYNC_WIDE, LIPSYNC_PUCKER, LIPSYNC_MID, LIPSYNC_SMALL, LIPSYNC_REST, LIPSYNC_REST,
        LIPSYNC_REST, LIPSYNC_REST,
    };
    return seq[(esp_timer_get_time() / 130000) % sizeof seq];
}
#endif

static void on_talk(lv_timer_t *t)
{
    (void)t;
    if (s_page < 0 || s_page >= s_count) return;
    photo_t *p = &s_photo[s_page];
    if (!p->chin_dsc.data) return;
#if CONFIG_BUDDY_LIPSYNC_DEMO
    int v = demo_viseme();
#else
    int v = lipsync_now();
#endif
    if (v < 0 || v >= LIPSYNC_COUNT) v = LIPSYNC_REST;
    /* Most of the way there in one 33 ms step: quick enough to keep up with speech,
     * soft enough that a one-frame flicker of the classifier does not snap the jaw. */
    p->open += (k_shape[v][0] / 100.0f - p->open) * 0.55f;
    p->width += (k_shape[v][1] / 100.0f - p->width) * 0.55f;
    mouth_place(p);
}

static void mouth_shut(photo_t *p)
{
    if (!p->chin_dsc.data) return;
    p->open = 0;
    p->width = 1;
    mouth_place(p);
}

/* The mouth's two objects, made once per photo and hidden until it is decoded. */
static void mouth_create(photo_t *p)
{
    lv_obj_remove_flag(p->img, LV_OBJ_FLAG_SCROLLABLE);

    p->hole = lv_obj_create(p->img);
    lv_obj_remove_style_all(p->hole);
    lv_obj_set_style_radius(p->hole, LV_RADIUS_CIRCLE, 0);
    /* Mostly dark, with a hint of upper teeth: a bright band reads as a sticker. */
    lv_obj_set_style_bg_opa(p->hole, 220, 0);
    lv_obj_set_style_bg_color(p->hole, lv_color_hex(0xb8aca4), 0);
    lv_obj_set_style_bg_grad_color(p->hole, lv_color_hex(0x1e0a0b), 0);
    lv_obj_set_style_bg_grad_dir(p->hole, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(p->hole, 0, 0);
    lv_obj_set_style_bg_grad_stop(p->hole, 70, 0);
    lv_obj_remove_flag(p->hole, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p->hole, LV_OBJ_FLAG_HIDDEN);

    /* After the hole, so it draws over it: the jaw's top edge is the lower lip. */
    p->chin = lv_image_create(p->img);
    lv_obj_remove_flag(p->chin, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p->chin, LV_OBJ_FLAG_HIDDEN);
}

static void decode(photo_t *p)
{
    if (p->dsc.data) return;
    int64_t t0 = esp_timer_get_time();

    /* 4 KB of work area, as LVGL's own TJpgDec glue uses: internal, it is hot. But with
     * voice up the internal heap can be down to a few KB in fragments, and a slower
     * decode beats a photo that never appears — so PSRAM if internal says no. */
    void *work = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!work) work = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!work) { ESP_LOGE(TAG, "no memory to decode %s", p->name); return; }

    jctx_t c = { .src = p->jpg, .len = p->size };
    JDEC jd;
    JRESULT r = jd_prepare(&jd, jd_in, work, 4096, &c);
    if (r != JDR_OK || jd.width == 0 || jd.height == 0 || jd.width > MAX_SIDE || jd.height > MAX_SIDE) {
        ESP_LOGE(TAG, "%s: not a JPEG this can show (%d, %ux%u) — progressive, or re-bake it",
                 p->name, (int)r, jd.width, jd.height);
        heap_caps_free(work);
        return;
    }

    size_t bytes = (size_t)jd.width * jd.height * 2;
    c.dst = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    c.w = jd.width;
    if (!c.dst) {
        ESP_LOGE(TAG, "no PSRAM for %s (%u B)", p->name, (unsigned)bytes);
        heap_caps_free(work);
        return;
    }
    r = jd_decomp(&jd, jd_out, 0);
    heap_caps_free(work);
    if (r != JDR_OK) {
        ESP_LOGE(TAG, "%s: decode failed (%d)", p->name, (int)r);
        heap_caps_free(c.dst);
        return;
    }

    p->dsc = (lv_image_dsc_t){
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                    .w = jd.width, .h = jd.height, .stride = jd.width * 2 },
        .data_size = bytes,
        .data = (const uint8_t *)c.dst,
    };
    lv_image_set_src(p->img, &p->dsc);
    chin_cut(p);
    ESP_LOGI(TAG, "%s: %ux%u decoded in %u ms", p->name, jd.width, jd.height,
             (unsigned)((esp_timer_get_time() - t0) / 1000));
}

static void chin_free(photo_t *p)
{
    if (!p->chin_dsc.data) return;
    lv_obj_add_flag(p->chin, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->hole, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(p->chin, NULL);
    lv_image_cache_drop(&p->chin_dsc);
    lv_image_header_cache_drop(&p->chin_dsc);
    heap_caps_free((void *)p->chin_dsc.data);
    p->chin_dsc.data = NULL;
}

static void release(photo_t *p)
{
    if (!p->dsc.data) return;
    chin_free(p);
    lv_image_set_src(p->img, NULL);
    /* The dsc is reused for the next decode at a new address; nothing may remember it. */
    lv_image_cache_drop(&p->dsc);
    lv_image_header_cache_drop(&p->dsc);
    heap_caps_free((void *)p->dsc.data);
    p->dsc.data = NULL;
}

/* ---- paging --------------------------------------------------------------- */

/* Page k is photo k. */
static void show_page(int page)
{
    if (page < 0 || page >= s_count) return;
    /* The photo swiped away stops mid-word rather than freezing with its mouth open. */
    if (s_page >= 0 && s_page < s_count && s_page != page) mouth_shut(&s_photo[s_page]);
    s_page = page;
    for (int i = 0; i < s_count; i++)
        if (i < page - 1 || i > page + 1) release(&s_photo[i]);
    /* The one in view first, so a slow neighbour never delays what was asked for. */
    decode(&s_photo[page]);
    for (int k = page - 1; k <= page + 1; k += 2)
        if (k >= 0 && k < s_count) decode(&s_photo[k]);
    /* Up to three photos are held at once; this is what that costs, page by page. */
    char stage[24];
    snprintf(stage, sizeof stage, "gallery p%d", page);
    mem_report(stage);

    lv_label_set_text(s_caption, s_photo[page].name);
    for (int i = 0; i < s_count; i++) {
        lv_obj_t *dot = lv_obj_get_child(s_dots, i);
        lv_obj_set_style_bg_opa(dot, i == page ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

static void on_page(lv_event_t *e)
{
    lv_obj_t *tile = lv_tileview_get_tile_active(lv_event_get_target(e));
    show_page((int)lv_obj_get_index(tile));
}

void gallery_create(lv_obj_t *scr)
{
    s_count = load_pack();
    if (s_count == 0) {
        /* A broken pack is a message on the glass, never a blank screen. */
        lv_obj_t *msg = lv_label_create(scr);
        lv_label_set_text(msg, "No photos.\nRe-bake gallery/gallery.bin.");
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(msg);
        return;
    }

    s_tv = lv_tileview_create(scr);
    lv_obj_set_size(s_tv, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_tv, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < s_count; i++) {
        lv_dir_t dir = (i > 0 ? LV_DIR_LEFT : 0) | (i + 1 < s_count ? LV_DIR_RIGHT : 0);
        lv_obj_t *tile = lv_tileview_add_tile(s_tv, (uint8_t)i, 0, dir);
        lv_obj_set_style_bg_color(tile, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        s_photo[i].img = lv_image_create(tile);
        lv_obj_center(s_photo[i].img);
        mouth_create(&s_photo[i]);
    }
    lv_obj_add_event_cb(s_tv, on_page, LV_EVENT_VALUE_CHANGED, NULL);

    /* Name and page dots, above the tileview so they hold still while pages slide.
     * Low on the glass but well inside the circle: the corners of a square do not
     * exist on a round panel. */
    s_caption = lv_label_create(scr);
    lv_obj_set_style_text_color(s_caption, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_caption, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_caption, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(s_caption, 12, 0);
    lv_obj_set_style_pad_ver(s_caption, 4, 0);
    lv_obj_set_style_radius(s_caption, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(s_caption, LV_ALIGN_BOTTOM_MID, 0, BOARD_ROUND ? -64 : -48);

    s_dots = lv_obj_create(scr);
    lv_obj_remove_style_all(s_dots);
    lv_obj_set_size(s_dots, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_dots, 6, 0);
    lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_dots, LV_ALIGN_BOTTOM_MID, 0, BOARD_ROUND ? -40 : -24);
    for (int i = 0; i < s_count; i++) {
        lv_obj_t *dot = lv_obj_create(s_dots);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_set_style_border_color(dot, lv_color_white(), 0);
        lv_obj_set_style_border_width(dot, 1, 0);
    }

    ESP_LOGI(TAG, "%d photos, %u B of JPEG", s_count, (unsigned)(pack_end - pack_start));
    show_page(0);
    /* 30 Hz: a viseme lasts 70 ms at the least (min_hold_ms), so every one is seen. */
    lv_timer_create(on_talk, 33, NULL);
}
