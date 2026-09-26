#pragma once
#include <stdint.h>
#include <stddef.h>

/** Render one frame at `elapsed_ms` into the framebuffer the caller owns. */
typedef void (*sim_render_cb)(uint32_t elapsed_ms);

/** Serve the framebuffer over HTTP until killed. Returns non-zero on a setup error. */
int sim_serve(int port, const uint16_t *fb, size_t fb_bytes, sim_render_cb render);
