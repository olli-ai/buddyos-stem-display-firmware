#pragma once
#include "sdkconfig.h"

/* Every buffer the PPA touches — source and destination — must start on a cache
 * line, and the destination's *stated size* must be a whole number of them: it
 * reads and writes PSRAM through the cache, and ppa_do_scale_rotate_mirror()
 * checks both. A plain heap_caps_malloc satisfies neither, and the only symptom
 * is ESP_ERR_INVALID_ARG, which says nothing about which argument.
 * esp_cache_get_alignment() would answer this at runtime but is private in
 * IDF 5.5, so take the larger of the two line sizes from Kconfig. */
#if defined(CONFIG_CACHE_L2_CACHE_LINE_SIZE) && \
    CONFIG_CACHE_L2_CACHE_LINE_SIZE > CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define FACE_PPA_ALIGN CONFIG_CACHE_L2_CACHE_LINE_SIZE
#elif defined(CONFIG_CACHE_L1_CACHE_LINE_SIZE)
#define FACE_PPA_ALIGN CONFIG_CACHE_L1_CACHE_LINE_SIZE
#else
#define FACE_PPA_ALIGN 64
#endif
#define FACE_PPA_ROUND(n) (((n) + FACE_PPA_ALIGN - 1) & ~((size_t)FACE_PPA_ALIGN - 1))

#include <stdint.h>
#include "esp_err.h"

/**
 * The P4's 2D accelerator, for the one operation LVGL cannot afford here: scaling a
 * full-screen image. See face_ppa.c for the measurements that led here.
 *
 * Source and destination are opaque RGB565 — the format face_view.c already flattens
 * the body into, and one of the few the PPA reads.
 */
esp_err_t face_ppa_start(void);

/** Scale `src` by `scale` into the middle of `dst`. Blocking; dst is not cleared. */
/* The body, moved as one piece into `dst`, with the strip the move uncovers filled
 * in `bg_rgb` (0xRRGGBB). Translation is exact to the pixel; scaling is not
 * available, and face_ppa.c says why. */
esp_err_t face_ppa_body(const void *src, uint32_t sw, uint32_t sh,
                        void *dst, uint32_t dw, uint32_t dh,
                        int tx, int ty, uint32_t bg_rgb);

esp_err_t face_ppa_scale(const void *src, uint32_t sw, uint32_t sh,
                         void *dst, uint32_t dw, uint32_t dh, float scale);

/** Time a few representative scales and log them. Proves the hardware before a
 *  compositor is designed around it. */
void face_ppa_benchmark(const void *src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh);
