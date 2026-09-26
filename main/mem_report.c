#include "mem_report.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "mem";

void mem_report(const char *stage)
{
    size_t i_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t i_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t i_blk  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t d_free = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    size_t d_blk  = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    /* Internal is the scarce one. DMA-capable internal is scarcer still, and it is
     * what the panel buffers and parts of the Wi-Fi path must come from — a big
     * `free` with a small largest-block is the shape of a failure that has not
     * happened yet. */
    ESP_LOGI(TAG, "[%s] internal: free %u, min-ever %u, largest %u | dma: free %u, largest %u",
             stage, (unsigned)i_free, (unsigned)i_min, (unsigned)i_blk,
             (unsigned)d_free, (unsigned)d_blk);

#if CONFIG_SPIRAM
    size_t p_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t p_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "[%s] psram:    free %u, min-ever %u", stage,
             (unsigned)p_free, (unsigned)p_min);
#else
    ESP_LOGW(TAG, "[%s] psram:    not enabled", stage);
#endif

    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    ESP_LOGI(TAG, "[%s] lvgl:     used %u of %u, peak %u, frag %u%%", stage,
             (unsigned)(m.total_size - m.free_size), (unsigned)m.total_size,
             (unsigned)m.max_used, (unsigned)m.frag_pct);
}
