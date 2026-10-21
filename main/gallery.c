/* Photo gallery. See gallery.h for the shape, tools/bake_gallery.ps1 for the pack. */
#include "gallery.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mem_report.h"

#include "src/libs/tjpgd/tjpgd.h"

static const char *TAG = "gallery";

#define MAX_PHOTOS 64
#define NAME_LEN   24
#define MAX_SIDE   480                 /* the panel; the baker never writes more */

/* The pack, little-endian like everything else here:
 *   "GLRY"  u32 count
 *   count x { u32 offset; u32 size; char name[24]; }   offsets from the pack's start
 *   the JPEGs
 * Embedded rather than downloaded, but read as if it were not: a stale or truncated
 * pack degrades to fewer photos, never to a fault. */
extern const uint8_t pack_start[] asm("_binary_gallery_bin_start");
extern const uint8_t pack_end[]   asm("_binary_gallery_bin_end");

typedef struct {
    const uint8_t *jpg;
    uint32_t size;
    char name[NAME_LEN];
    lv_obj_t *img;
    lv_image_dsc_t dsc;                /* .data is NULL while not decoded */
} photo_t;

/* PSRAM: 3.8 KB that is read once per swipe, and internal SRAM is what voice is short of. */
EXT_RAM_BSS_ATTR static photo_t s_photo[MAX_PHOTOS];
static int s_count;
static lv_obj_t *s_tv, *s_caption, *s_dots;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int load_pack(void)
{
    size_t len = (size_t)(pack_end - pack_start);
    if (len < 8 || memcmp(pack_start, "GLRY", 4) != 0) {
        ESP_LOGW(TAG, "no gallery pack (run tools/bake_gallery.ps1)");
        return 0;
    }
    uint32_t n = rd32(pack_start + 4);
    if (n > MAX_PHOTOS) n = MAX_PHOTOS;
    int kept = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = pack_start + 8 + i * (8 + NAME_LEN);
        if (e + 8 + NAME_LEN > pack_end) break;
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
    ESP_LOGI(TAG, "%s: %ux%u decoded in %u ms", p->name, jd.width, jd.height,
             (unsigned)((esp_timer_get_time() - t0) / 1000));
}

static void release(photo_t *p)
{
    if (!p->dsc.data) return;
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
    lv_obj_align(s_caption, LV_ALIGN_BOTTOM_MID, 0, -64);

    s_dots = lv_obj_create(scr);
    lv_obj_remove_style_all(s_dots);
    lv_obj_set_size(s_dots, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_dots, 6, 0);
    lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_dots, LV_ALIGN_BOTTOM_MID, 0, -40);
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
}
