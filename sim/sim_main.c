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
#include "face_lipsync.h"
#include "face_catalogue.h"
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

/* ---- the glass's angle, on the host ---------------------------------------
 *
 * On the device this lives in NVS and is set from the website (main/face_rot.c),
 * which wants a flash partition the host has not got -- so face_view.c linked
 * against nothing here and the simulator stopped building when the angle stopped
 * being a compile-time constant. It is an environment variable instead:
 *
 *   FACE_ROT=-48 bash sim/build.sh sim/frames
 *
 * Worth having rather than hard-coding 0: the sign of this number is the thing
 * everyone gets wrong (it turns the FACE, so it opposes the case), and a picture
 * settles it in a second where reasoning about it has already failed once.
 */
int face_rot_deg(void)
{
    static int deg = -1000;
    if (deg == -1000) {
        const char *e = getenv("FACE_ROT");
        deg = e ? atoi(e) : 0;
        if (deg < -180 || deg > 180) deg = 0;
    }
    return deg;
}

/* ---- lip-sync --------------------------------------------------------------
 * The speaking state is the one worth watching, and until this device has audio
 * hardware the only way to watch it is to play the same clip the conformance check
 * uses through the same classifier the firmware runs. The audio clock is the sim
 * clock: a frame at t ms sees exactly t ms of the clip, so a paused simulator shows
 * a real mouth shape for a real instant of speech rather than a plausible one.
 */
static int16_t *s_pcm;
static size_t s_pcm_n, s_pcm_at;

static void load_fixture(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;                     /* no clip, no lip-sync; everything else runs */
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    s_pcm = malloc((size_t)bytes);
    s_pcm_n = fread(s_pcm, sizeof(int16_t), (size_t)bytes / 2, f);
    fclose(f);
    printf("lip-sync     : %s, %.1f s at %d Hz\n", path, (double)s_pcm_n / face_viseme_rate, face_viseme_rate);
}

static void lipsync_at(uint32_t ms)
{
    /* Which state is showing is asked of the renderer rather than tracked here: in
     * serve mode the *server* cycles the states, and a flag set by the frame-dump
     * loop would leave the browser — the thing anyone actually watches — showing a
     * speaking Buddy with a silent mouth. */
    static int last = -1;
    static uint32_t entered;
    int state = face_view_state();
    if (state != last) { last = state; entered = ms; face_lipsync_start(); s_pcm_at = 0; }

    if (!s_pcm || strcmp(face_states[state].name, "speaking") != 0) {
        face_view_set_live_mouth(-1);
        return;
    }

    /* Time since this state began, so the clip restarts with each turn at speaking. */
    size_t want = (size_t)((uint64_t)(ms - entered) * (uint64_t)face_viseme_rate / 1000u);
    if (want > s_pcm_n) want = s_pcm_n;
    if (want < s_pcm_at) { face_lipsync_start(); s_pcm_at = 0; }
    int16_t sprite = -1;
    while (s_pcm_at < want) {
        size_t take = want - s_pcm_at < 256 ? want - s_pcm_at : 256;
        sprite = face_lipsync_feed(&s_pcm[s_pcm_at], take);
        s_pcm_at += take;
    }
    /* Past the end of the clip the Buddy has stopped talking, and a mouth left on
     * its last syllable is exactly the artefact the stop path exists to avoid. */
    if (s_pcm_at >= s_pcm_n) { face_lipsync_stop(); sprite = -1; }
    face_view_set_live_mouth(sprite);
}

/* One frame at a given moment, straight into fb. Shared by both modes so what you
 * watch in a browser is what the frame dumps show. */
static void render_at(uint32_t ms)
{
    sim_ms = ms;
    lipsync_at(ms);
    face_view_tick(ms);
    lv_refr_now(s_disp);
}

/* ---- wearing a catalogue character, on the host -----------------------------
 *
 * The device reads `faces/face_catalogue.bin` out of a memory-mapped partition.
 * There is no partition here, but the file is the same bytes, so reading it into
 * a buffer and pointing the same descriptors at it exercises the same code.
 *
 * This is not a nicety. Everything about a *catalogue* character — its own eyes
 * and mouth, and the factor between the size it was baked at and the panel — was
 * invisible to this simulator, which rendered only the built-in face where that
 * factor is exactly 1. A placement error that is identically zero on the built-in
 * character and 15 px on Sprout could not be seen here at all, and was not. */
#define CAT_HDR 16
#define CAT_REC 96

static face_character_t s_sim_char;

static uint16_t cat_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t cat_rd32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void cat_describe(lv_image_dsc_t *d, const uint8_t *base, lv_color_format_t cf,
                         uint16_t w, uint16_t h, uint32_t off, uint32_t size)
{
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = cf;
    d->header.w = w;
    d->header.h = h;
    d->header.stride = (uint32_t)w * (cf == LV_COLOR_FORMAT_ARGB8888 ? 4 : 2);
    d->data_size = size;
    d->data = base + off;
}

/** Load one character out of a catalogue image on disk. NULL if it is not in it. */
static const face_character_t *sim_load_character(const char *path, const char *id)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("character    : no catalogue at %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *map = malloc((size_t)n);               /* freed by exiting */
    if (!map || fread(map, 1, (size_t)n, f) != (size_t)n) { fclose(f); return NULL; }
    fclose(f);
    if (n < CAT_HDR || memcmp(map, "BUDDYCAT", 8)) { printf("character    : not a catalogue\n"); return NULL; }

    int count = cat_rd16(map + 10);
    for (int i = 0; i < count; i++) {
        const uint8_t *r = map + CAT_HDR + (size_t)i * CAT_REC;
        if (strncmp((const char *)r, id, 15)) continue;
        face_character_t *c = &s_sim_char;
        memset(c, 0, sizeof(*c));
        memcpy(c->id, r, 15);
        cat_describe(&c->bg, map, LV_COLOR_FORMAT_RGB565A8,
                     cat_rd16(r + 16), cat_rd16(r + 18), cat_rd32(r + 20), cat_rd32(r + 24));
        for (int k = 0; k < 3; k++) {
            const uint8_t *a = r + 28 + k * 20;
            uint16_t w = cat_rd16(a), h = cat_rd16(a + 2);
            if (!w || !h) continue;
            c->art_x[k] = (int16_t)cat_rd16(a + 4);
            c->art_y[k] = (int16_t)cat_rd16(a + 6);
            cat_describe(&c->art[k], map, LV_COLOR_FORMAT_ARGB8888, w, h,
                         cat_rd32(a + 8), cat_rd32(a + 12));
        }
        return c;
    }
    printf("character    : '%s' is not in the catalogue\n", id);
    return NULL;
}

int main(int argc, char **argv)
{
    /*  facesim --serve [port]   watch it animate in a browser
     *  facesim <dir>            dump one PPM per sampled moment            */
    int serve = argc > 1 && strcmp(argv[1], "--serve") == 0;
    int port = (serve && argc > 2) ? atoi(argv[2]) : 8080;
    if (port <= 0) port = 8080;         /* an empty or junk argument is not "any port" */
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
    load_fixture(getenv("FACE_PCM") ? getenv("FACE_PCM") : "tools/lipsync-fixture.pcm");

    /* FACE_BODY=sky renders the Buddy in a chosen body colour, the way a design
     * asking for one would. Here because the tint is a rendering question and this
     * is where rendering questions get looked at rather than reasoned about. */
    const char *body = getenv("FACE_BODY");
    if (body)
        for (int i = 0; i < face_body_color_count; i++)
            if (!strcmp(face_body_colors[i].id, body)) {
                face_view_set_body_color(face_body_colors[i].rgb, !strcmp(body, "original"));
                printf("body colour  : %s\n", body);
            }

    /* FACE_CHARACTER=sprout wears one of the catalogue's characters, body and the
     * face its artist drew, exactly as a design naming it would. */
    const char *who = getenv("FACE_CHARACTER");
    if (who) {
        const char *cat = getenv("FACE_CATALOGUE");
        const face_character_t *c = sim_load_character(cat ? cat : "faces/face_catalogue.bin", who);
        if (c) {
            face_view_set_character(c);
            /* ...wearing its own face, which is what a design naming a character
             * asks for and what the kit on the bench actually shows: the eyes and
             * the mouth its artist drew, not the firmware's drawn parts. */
            const int16_t own_face[5] = { -1, FACE_PART_ARTWORK, -1,
                                          FACE_PART_ARTWORK, FACE_PART_ARTWORK };
            for (int st = 0; st < face_state_count && st < 8; st++)
                face_view_set_state_parts(st, own_face);
            printf("character    : %s, baked at %dx%d for a %d panel\n",
                   c->id, (int)c->bg.header.w, (int)c->bg.header.h, W);
        }
    }

    /* FACE_ACCESSORY=cap [FACE_ACCENT=pink] puts on what a child picked. Where the
     * body colour is a question about pixels, this one is about *placement* — does
     * the cap sit on the head, do the glasses land on the eyes — and placement is
     * precisely what this simulator is trusted for. */
    const char *acc = getenv("FACE_ACCESSORY");
    if (acc) {
        const char *tint = getenv("FACE_ACCENT");
        uint32_t rgb = face_accessory_colors[0].rgb;
        for (int i = 0; tint && i < face_accessory_color_count; i++)
            if (!strcmp(face_accessory_colors[i].id, tint)) rgb = face_accessory_colors[i].rgb;
        face_view_set_accessory(acc, rgb);
        printf("accessory    : %s in #%06X\n", acc, (unsigned)rgb);
    }

    /* What this would cost the device. Container limits cannot model an MCU — a
     * Linux process is megabytes before it does anything — so the budget worth
     * watching is LVGL's own, which is capped at the same LV_MEM_SIZE the firmware
     * will use. Sprite data is const and lives in flash, not RAM. */
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    size_t sprite_bytes = face_bg.data_size;
    for (int i = 0; i < face_sprite_count; i++)
        if (face_sprites[i].img) sprite_bytes += face_sprites[i].img->data_size;
    /* The accessory masks are part of the artwork bill: alpha only, which is the
     * point of baking them that way. */
    size_t acc_bytes = 0;
    for (int i = 0; i < face_accessory_count; i++) {
        if (face_accessories[i].accent) acc_bytes += face_accessories[i].accent->data_size;
        if (face_accessories[i].detail) acc_bytes += face_accessories[i].detail->data_size;
    }
    sprite_bytes += acc_bytes;
    printf("draw buffers : 2 x %d px RGB565 = %zu B (RAM, as main.c configures)\n",
           BUF_PX, sizeof(buf1) + sizeof(buf2));
    printf("lvgl heap    : %u B total, %u B used after setup\n",
           (unsigned)m.total_size, (unsigned)(m.total_size - m.free_size));
    printf("sprites      : %zu B (flash/.rodata, not RAM)\n", sprite_bytes);
    printf("accessories  : %zu B of that, %d of them\n", acc_bytes, face_accessory_count - 1);
    fflush(stdout);

    if (serve) {
        sim_stats_t st = { .draw_buffer_bytes = sizeof(buf1) + sizeof(buf2),
                           .sprite_bytes = sprite_bytes, .buf_px = BUF_PX };
        return sim_serve(port, fb, sizeof(fb), render_at, &st);
    }

    /* ---- film mode: one continuous take ------------------------------------
     *
     * The sampled moments below are for inspection -- one frame at the peak of each
     * loop, which is what you want when judging a pose and useless when judging
     * motion. FACE_FILM scripts a run of states instead and dumps every frame of it:
     *
     *   FACE_FILM=idle:2000,listening:1500,speaking:3000 FACE_FILM_FPS=20 \
     *       bash sim/build.sh sim/film
     *
     * The clock runs on across the whole take, so each state's loop starts where the
     * real thing would rather than from zero, and `speaking` lip-syncs to the same
     * fixture clip the conformance check uses. Frames land as film-0000.ppm upwards,
     * which is the numbering ffmpeg reads with -i film-%04d.ppm.
     */
    const char *film = getenv("FACE_FILM");
    if (film) {
        const char *fps_env = getenv("FACE_FILM_FPS");
        unsigned fps = fps_env ? (unsigned)atoi(fps_env) : 20;
        if (fps < 1 || fps > 60) fps = 20;
        const uint32_t step = 1000u / fps;
        uint32_t now = 0;
        unsigned n = 0;
        char script[512];
        snprintf(script, sizeof(script), "%s", film);
        for (char *tok = strtok(script, ","); tok; tok = strtok(NULL, ",")) {
            char *colon = strchr(tok, ':');
            unsigned hold = colon ? (unsigned)atoi(colon + 1) : 2000;
            if (colon) *colon = 0;
            int state = -1;
            for (int i = 0; i < face_state_count; i++)
                if (!strcmp(face_states[i].name, tok)) state = i;
            if (state < 0) { fprintf(stderr, "no such state: %s\n", tok); return 1; }
            face_view_set_state(state);
            for (uint32_t t = 0; t < hold; t += step, now += step) {
                render_at(now);
                char name[128];
                snprintf(name, sizeof(name), "film-%04u", n++);
                dump(out, name);
            }
            printf("filmed %s for %u ms\n", tok, hold);
        }
        printf("%u frames at %u fps into %s\n", n, fps, out);
        return 0;
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
    /* A strip through the clip: what a child would see while the Buddy says one
     * bilingual sentence. Spaced at the hop, so consecutive frames are consecutive
     * decisions and a shape that never changes — or changes every frame — shows up. */
    if (s_pcm) {
        for (int i = 0; i < face_state_count; i++)
            if (!strcmp(face_states[i].name, "speaking")) face_view_set_state(i);
        for (unsigned ms = 0; ms < 2400; ms += 48) {
            render_at(ms);
            char name[128];
            snprintf(name, sizeof(name), "talk-%04u", ms);
            dump(out, name);
        }
        printf("rendered 50 lip-sync frames over 2.4 s of real speech\n");
    }
    printf("done\n");
    return 0;
}
