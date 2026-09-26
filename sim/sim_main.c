/*
 * Headless LVGL simulator for the face.
 *
 * There is no panel on this desk, and a picture of the face is the only thing that
 * can say whether the translation worked. So LVGL is built for the host, given a
 * plain memory buffer instead of a GC9A01, and asked to render the same
 * face_view.c / face_motion.c / face_assets.c the firmware runs. Frames come out as
 * PPM files that can be looked at.
 *
 * What this does NOT cover: the SPI bus, the panel driver, and anything about timing
 * on real silicon. Everything above that line is the real code.
 */
#include "lvgl.h"
#include "face_view.h"
#include "face_motion.h"
#include "sim_server.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define W 240
#define H 240

/* The "panel": what a GC9A01 would be holding. LVGL never draws into this — the
 * flush callback pushes into it, exactly as the SPI driver would. */
static uint16_t fb[W * H];

/* What LVGL actually draws into, and the number that matters.
 *
 * main.c gives the device two partial buffers of a tenth of the screen. The first
 * version of this simulator handed LVGL one full-screen buffer in DIRECT mode, which
 * is 115 KB and five times what the device has — so it was not exercising the device's
 * draw path at all. Partial rendering redraws each dirty area in chunks, and chunked
 * drawing of transformed images is precisely where LVGL behaves differently. Matching
 * the device here is the whole point of a simulator. */
#define BUF_PX (W * H / 10)
static uint16_t buf1[BUF_PX];
static uint16_t buf2[BUF_PX];

static uint32_t sim_ms;

static uint32_t tick_cb(void) { return sim_ms; }

/* Stand in for the SPI driver: copy the rendered area into the panel's memory. */
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const uint16_t *src = (const uint16_t *)px;
    int w = a->x2 - a->x1 + 1;
    for (int y = a->y1; y <= a->y2; y++) {
        if (y < 0 || y >= H) continue;
        memcpy(&fb[y * W + a->x1], src, (size_t)w * 2);
        src += w;
    }
    lv_display_flush_ready(d);
}

static void dump(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint16_t v = fb[i];
        uint8_t rgb[3] = {
            (uint8_t)(((v >> 11) & 0x1f) << 3),
            (uint8_t)(((v >> 5) & 0x3f) << 2),
            (uint8_t)((v & 0x1f) << 3),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static lv_display_t *s_disp;

/* One frame at a given moment, straight into fb. Shared by both modes so what you
 * watch in a browser is what the frame dumps show. */
static void render_at(uint32_t ms)
{
    sim_ms = ms;
    face_view_tick(ms);
    lv_refr_now(s_disp);
}

int main(int argc, char **argv)
{
    /*  facesim --serve [port]   watch it animate in a browser
     *  facesim <dir>            dump one PPM per sampled moment            */
    int serve = argc > 1 && strcmp(argv[1], "--serve") == 0;
    int port = (serve && argc > 2) ? atoi(argv[2]) : 8080;
    const char *out = (!serve && argc > 1) ? argv[1] : ".";

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *d = s_disp = lv_display_create(W, H);
    lv_display_set_buffers(d, buf1, buf2, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);

    lv_obj_t *scr = lv_display_get_screen_active(d);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    face_view_create(scr);

    /* What this would cost the device. Container limits cannot model an MCU — a
     * Linux process is megabytes before it does anything — so the budget worth
     * watching is LVGL's own, which is capped at the same LV_MEM_SIZE the firmware
     * will use. Sprite data is const and lives in flash, not RAM. */
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    size_t sprite_bytes = face_bg.data_size;
    for (int i = 0; i < face_sprite_count; i++)
        if (face_sprites[i].img) sprite_bytes += face_sprites[i].img->data_size;
    printf("draw buffers : 2 x %d px RGB565 = %zu B (RAM, as main.c configures)\n",
           BUF_PX, sizeof(buf1) + sizeof(buf2));
    printf("lvgl heap    : %u B total, %u B used after setup\n",
           (unsigned)m.total_size, (unsigned)(m.total_size - m.free_size));
    printf("sprites      : %zu B (flash/.rodata, not RAM)\n", sprite_bytes);
    fflush(stdout);

    if (serve) {
        sim_stats_t st = { .draw_buffer_bytes = sizeof(buf1) + sizeof(buf2),
                           .sprite_bytes = sprite_bytes, .buf_px = BUF_PX };
        return sim_serve(port, fb, sizeof(fb), render_at, &st);
    }

    /* Moments chosen to land on the interesting part of each loop: mid-blink, the
     * peak of a breath, a brow at the top of its overshoot, the mouth mid-syllable. */
    static const uint32_t TS[] = { 0, 120, 240, 400, 700, 1500, 3650, 3700 };

    for (int s = 0; s < face_state_count; s++) {
        face_view_set_state(s);
        for (unsigned i = 0; i < sizeof(TS) / sizeof(TS[0]); i++) {
            render_at(TS[i]);
            char name[128];
            snprintf(name, sizeof(name), "%s-%04u", face_states[s].name, (unsigned)TS[i]);
            dump(out, name);
        }
        printf("rendered %s (%u ms loop)\n", face_states[s].name, face_states[s].duration);
    }
    printf("done\n");
    return 0;
}
