/*
 * The P4's 2D accelerator, measured before anything is built on it.
 *
 * LVGL on this panel cannot animate a whole face: 518,400 pixels scaled in software
 * costs 250-460 ms a frame, and even a plain opaque full-screen copy through its
 * blit path costs 48-62 ms, which is 16 fps before any scaling at all. Its hardware
 * draw units are nxp, renesas and vg_lite — there is none for Espressif — so on this
 * chip LVGL is software by construction.
 *
 * The PPA is the hardware that does exactly the missing operation: scale-rotate-
 * mirror over DMA, straight into a frame buffer. This file exists to find out what
 * it costs here, on this board, with this image — a number, before a compositor is
 * designed around it.
 *
 * Measured on the Waveshare P4 board, 720x720 RGB565 in PSRAM (boot log):
 *
 *     x1.000  14.1 ms      x0.960  14.1 ms      x1.020  15.0 ms
 *     x1.080  13.1 ms      x1.875   6.4 ms
 *
 * So ~37 Mpixel/s, and the cost follows the *source* window read, not the output
 * written — which is why x1.875 is the cheapest of them: it reads a 384x384 crop.
 * A whole-face transform therefore fits inside a 16.7 ms frame, where LVGL needed
 * 250-460 ms. That is the number the compositor is built on.
 *
 * Known limits, worth stating before anyone plans on it: the PPA rotates in quarter
 * turns only (PPA_SRM_ROTATION_ANGLE_0/90/180/270), so a head *tilt* can never be
 * hardware, and it reads RGB565/RGB888/ARGB8888 but not LVGL's planar RGB565A8 —
 * which is why face_view.c flattens the body to opaque RGB565 first.
 */
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32P4

#include "face_ppa.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "face_ppa";

static ppa_client_handle_t s_srm;
static ppa_client_handle_t s_fill;      /* a fill is a different client, not a flag */
static size_t s_dst_size;   /* the aligned size the PPA was told about */

esp_err_t face_ppa_start(void)
{
    if (s_srm) return ESP_OK;
    ppa_client_config_t cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    esp_err_t err = ppa_register_client(&cfg, &s_srm);
    if (err != ESP_OK) { ESP_LOGE(TAG, "no PPA SRM client: %s", esp_err_to_name(err)); return err; }

    cfg.oper_type = PPA_OPERATION_FILL;
    err = ppa_register_client(&cfg, &s_fill);
    if (err != ESP_OK) ESP_LOGE(TAG, "no PPA fill client: %s", esp_err_to_name(err));
    return err;
}

/* One flat rectangle, for the strip a move uncovers. */
static esp_err_t fill(void *dst, uint32_t dw, uint32_t dh,
                      uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    if (!w || !h) return ESP_OK;
    ppa_fill_oper_config_t f = {
        .out = {
            .buffer = dst,
            .buffer_size = FACE_PPA_ROUND((size_t)dw * dh * 2),
            .pic_w = dw, .pic_h = dh,
            .block_offset_x = x, .block_offset_y = y,
            .fill_cm = PPA_FILL_COLOR_MODE_RGB565,
        },
        .fill_block_w = w, .fill_block_h = h,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    f.fill_argb_color.val = 0xFF000000u | (rgb & 0xFFFFFFu);
    return ppa_do_fill(s_fill, &f);
}

/* How many output pixels one input pixel becomes, in sixteenths — the only form the
 * hardware has. 16 is 1:1; 30 is the 1.875 that turns a 384 bake into this panel. */
static uint32_t out_of(uint32_t block, uint32_t k16)
{
    return block * (k16 / 16) + block * (k16 % 16) / 16;
}

/* Put the whole body on the panel, moved as one piece and scaled from whatever size
 * it was baked at.
 *
 * Translation is exact to the output pixel. Scale is not free-form — PPA_SR_SCAL_X_FRAG
 * is four bits, so the ratio must be a whole number of sixteenths — but the one ratio
 * this needs, 720/384, is exactly 30/16, and 1:1 is 16/16. A size that does not land
 * on a sixteenth is refused rather than silently drawn at the wrong scale.
 *
 * Rotation is NOT here and cannot be: the hardware turns in quarter circles only. A
 * turned face is a separate pre-rendered source handed to this function (face_view.c),
 * which is why the source size is a parameter rather than an assumption.
 *
 * Whatever the move leaves uncovered is filled with the body's own background, as four
 * rectangles around what was written. Four rather than two because a scaled block can
 * fall a pixel or two short of the panel, and a seam of stale frame is worse than a
 * fill that overlaps itself. */
esp_err_t face_ppa_body(const void *src, uint32_t sw, uint32_t sh,
                        void *dst, uint32_t dw, uint32_t dh,
                        int tx, int ty, uint32_t bg_rgb)
{
    if (!s_srm || !s_fill || !sw || !sh) return ESP_ERR_INVALID_STATE;
    uint32_t kx = (dw * 16 + sw / 2) / sw, ky = (dh * 16 + sh / 2) / sh;
    if (kx != ky || kx < 16 || kx > 255) return ESP_ERR_INVALID_ARG;
    if (out_of(sw, kx) != dw || out_of(sh, kx) != dh) return ESP_ERR_INVALID_ARG;

    uint32_t ax = (uint32_t)(tx < 0 ? -tx : tx), ay = (uint32_t)(ty < 0 ? -ty : ty);
    if (ax >= dw || ay >= dh) return ESP_ERR_INVALID_ARG;   /* moved clean off the panel */

    /* The largest source block whose scaled width still fits the space the move left. */
    uint32_t bw = (dw - ax) * 16 / kx, bh = (dh - ay) * 16 / kx;
    while (bw > 1 && out_of(bw, kx) > dw - ax) bw--;
    while (bh > 1 && out_of(bh, kx) > dh - ay) bh--;
    if (bw > sw) bw = sw;
    if (bh > sh) bh = sh;

    uint32_t in_x = tx < 0 ? (ax * 16 + kx - 1) / kx : 0;
    uint32_t in_y = ty < 0 ? (ay * 16 + kx - 1) / kx : 0;
    if (in_x + bw > sw) in_x = sw - bw;
    if (in_y + bh > sh) in_y = sh - bh;

    uint32_t ox = tx > 0 ? ax : 0, oy = ty > 0 ? ay : 0;
    uint32_t ow = out_of(bw, kx), oh = out_of(bh, kx);

    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = (void *)src,
            .pic_w = sw, .pic_h = sh,
            .block_w = bw, .block_h = bh,
            .block_offset_x = in_x, .block_offset_y = in_y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dst,
            .buffer_size = FACE_PPA_ROUND((size_t)dw * dh * 2),
            .pic_w = dw, .pic_h = dh,
            .block_offset_x = ox, .block_offset_y = oy,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = kx / 16.0f, .scale_y = kx / 16.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = ppa_do_scale_rotate_mirror(s_srm, &op);
    if (err != ESP_OK) return err;

    /* What the hardware half of a frame costs, against what the frame costs — the
     * two numbers together say whether the body or LVGL is the one to work on. */
    static uint32_t n; static int64_t total;
    total += esp_timer_get_time() - t0;
    if (++n == 100) {
        ESP_LOGI(TAG, "100 body blits, mean %lld us", total / 100);
        n = 0; total = 0;
    }

    if (ox)           err = fill(dst, dw, dh, 0, 0, ox, dh, bg_rgb);
    if (err == ESP_OK && ox + ow < dw)
        err = fill(dst, dw, dh, ox + ow, 0, dw - ox - ow, dh, bg_rgb);
    if (err == ESP_OK && oy) err = fill(dst, dw, dh, 0, 0, dw, oy, bg_rgb);
    if (err == ESP_OK && oy + oh < dh)
        err = fill(dst, dw, dh, 0, oy + oh, dw, dh - oy - oh, bg_rgb);
    return err;
}

esp_err_t face_ppa_scale(const void *src, uint32_t sw, uint32_t sh,
                         void *dst, uint32_t dw, uint32_t dh, float scale)
{
    if (!s_srm) return ESP_ERR_INVALID_STATE;

    /* The PPA scales a *block* of the source into a block of the destination and
     * refuses one that would overrun it. So growing the face means reading a
     * smaller window of it, not writing a bigger picture: a centred crop of
     * dw/scale pixels, blown up to fill the panel, is exactly the face grown about
     * its middle and clipped at the bezel — which is what a breath looks like.
     * Shrinking is the other way round: the whole source into a centred block,
     * with the margin left as it was. */
    uint32_t bw = sw, bh = sh;
    if (scale > 1.0f) {
        bw = (uint32_t)(dw / scale);
        bh = (uint32_t)(dh / scale);
        /* The driver floors scale*block itself; step down until its sum fits. */
        while (bw > 1 && (uint32_t)(bw * scale) > dw) bw--;
        while (bh > 1 && (uint32_t)(bh * scale) > dh) bh--;
        if (bw > sw) bw = sw;
        if (bh > sh) bh = sh;
    }
    uint32_t ow = (uint32_t)(bw * scale), oh = (uint32_t)(bh * scale);
    if (ow > dw || oh > dh) return ESP_ERR_INVALID_ARG;

    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = src,
            .pic_w = sw, .pic_h = sh,
            .block_w = bw, .block_h = bh,
            .block_offset_x = (sw - bw) / 2,
            .block_offset_y = (sh - bh) / 2,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dst,
            .buffer_size = s_dst_size ? s_dst_size : FACE_PPA_ROUND(dw * dh * 2),
            .pic_w = dw, .pic_h = dh,
            .block_offset_x = (dw - ow) / 2,
            .block_offset_y = (dh - oh) / 2,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = scale, .scale_y = scale,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_srm, &op);
}

void face_ppa_benchmark(const void *src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
    if (face_ppa_start() != ESP_OK) return;
    size_t want = FACE_PPA_ROUND(dw * dh * 2);   /* see FACE_PPA_ALIGN */
    void *dst = heap_caps_aligned_alloc(FACE_PPA_ALIGN, want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dst) { ESP_LOGW(TAG, "no PSRAM for the benchmark target"); return; }
    s_dst_size = want;

    /* Scales a breath crosses, and the one a catalogue character needs anyway. */
    static const float at[] = { 1.00f, 0.96f, 1.02f, 1.08f, 1.875f };
    for (int i = 0; i < (int)(sizeof at / sizeof *at); i++) {
        int64_t t0 = esp_timer_get_time();
        esp_err_t err = face_ppa_scale(src, sw, sh, dst, dw, dh, at[i]);
        int64_t us = esp_timer_get_time() - t0;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "scale x%.2f: %s", (double)at[i], esp_err_to_name(err));
            continue;
        }
        ESP_LOGI(TAG, "scale x%.3f of %lux%lu into %lux%lu: %lld us  (LVGL does this in 250000-460000)",
                 (double)at[i], (unsigned long)sw, (unsigned long)sh,
                 (unsigned long)dw, (unsigned long)dh, us);
    }
    heap_caps_free(dst);
}

#endif /* CONFIG_IDF_TARGET_ESP32P4 */
