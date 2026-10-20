#pragma once
#include <stdint.h>
#include <stddef.h>

/** Render one frame at `elapsed_ms` into the framebuffer the caller owns. */
typedef void (*sim_render_cb)(uint32_t elapsed_ms);

/** What this face would cost the device. The static half is measured once at
 *  startup; the live half (LVGL heap, frame time) is read per request. */
typedef struct {
    size_t draw_buffer_bytes;   /* RAM: LVGL's partial draw buffer */
    size_t sprite_bytes;        /* flash/.rodata: the baked catalogue, not RAM */
    int    buf_px;              /* pixels per draw buffer */
} sim_stats_t;

/** Serve the framebuffer over HTTP until killed. Returns non-zero on a setup error. */
int sim_serve(int port, const uint16_t *fb, size_t fb_bytes, sim_render_cb render,
              const sim_stats_t *stats);
