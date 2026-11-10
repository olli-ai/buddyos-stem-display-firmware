/* Host stand-in so face_view.c compiles unchanged in the simulator.
 *
 * There is one heap here and no PSRAM, so the capability bits are 0 and every
 * allocation is the C library's. The alignment IS honoured: the P4 path asks for
 * cache-line-aligned buffers, and a simulator that quietly ignored that would stop
 * being the same code. */
#pragma once
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM   0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_8BIT     0
#define MALLOC_CAP_DMA      0

static inline void *heap_caps_malloc(size_t n, uint32_t caps) { (void)caps; return malloc(n); }
static inline void heap_caps_free(void *p) { free(p); }
static inline void *heap_caps_aligned_alloc(size_t align, size_t n, uint32_t caps)
{
    (void)caps;
    void *p = NULL;
    if (align < sizeof(void *)) align = sizeof(void *);
    return posix_memalign(&p, align, n) ? NULL : p;
}
static inline size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 0; }
