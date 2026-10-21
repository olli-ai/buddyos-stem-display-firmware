#include "face_catalogue.h"
#include "esp_partition.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "catalogue";

#define MAGIC   "BUDDYCAT"
#define VERSION 1
#define HDR     16
#define REC     96
#define MAX_CHARACTERS 12

static const uint8_t *s_map;            /* the partition, mapped into the address space */
static esp_partition_mmap_handle_t s_handle;
static face_character_t s_chars[MAX_CHARACTERS];
static int s_count;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/** An image descriptor pointing into mapped flash. Nothing is copied. */
static void describe(lv_image_dsc_t *d, lv_color_format_t cf, uint16_t w, uint16_t h,
                     uint32_t off, uint32_t size)
{
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = cf;
    d->header.w = w;
    d->header.h = h;
    d->header.stride = (uint32_t)w * (cf == LV_COLOR_FORMAT_ARGB8888 ? 4 : 2);
    d->data_size = size;
    d->data = s_map + off;
}

bool face_catalogue_mount(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "faces");
    if (!part) { ESP_LOGW(TAG, "no 'faces' partition; using the built-in character"); return false; }

    const void *mapped = NULL;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &mapped, &s_handle);
    if (err != ESP_OK) { ESP_LOGE(TAG, "mmap: %s", esp_err_to_name(err)); return false; }
    s_map = (const uint8_t *)mapped;

    if (memcmp(s_map, MAGIC, 8)) {
        /* Blank flash reads as 0xFF, which is the usual reason — the catalogue has
         * simply not been written yet. Not an error, and not a reason to stop. */
        ESP_LOGW(TAG, "no catalogue written (%02x%02x…); using the built-in character",
                 s_map[0], s_map[1]);
        s_map = NULL;
        return false;
    }
    uint16_t version = rd16(s_map + 8);
    if (version != VERSION) {
        ESP_LOGE(TAG, "catalogue is version %u, this firmware reads %u", version, VERSION);
        s_map = NULL;
        return false;
    }

    int n = rd16(s_map + 10);
    if (n > MAX_CHARACTERS) n = MAX_CHARACTERS;
    for (int i = 0; i < n; i++) {
        const uint8_t *r = s_map + HDR + (size_t)i * REC;
        face_character_t *c = &s_chars[i];
        memcpy(c->id, r, 15);
        c->id[15] = 0;
        describe(&c->bg, LV_COLOR_FORMAT_RGB565A8, rd16(r + 16), rd16(r + 18), rd32(r + 20), rd32(r + 24));
        for (int k = 0; k < 3; k++) {
            const uint8_t *a = r + 28 + k * 20;
            uint16_t w = rd16(a), h = rd16(a + 2);
            if (!w || !h) continue;
            c->art_x[k] = (int16_t)rd16(a + 4);
            c->art_y[k] = (int16_t)rd16(a + 6);
            describe(&c->art[k], LV_COLOR_FORMAT_ARGB8888, w, h, rd32(a + 8), rd32(a + 12));
        }
    }
    s_count = n;
    ESP_LOGI(TAG, "%d characters mapped from flash, %u KB, no RAM", n, (unsigned)(rd32(s_map + 12) / 1024));
    for (int i = 0; i < n; i++) ESP_LOGI(TAG, "  %s", s_chars[i].id);
    return true;
}

int face_catalogue_count(void) { return s_count; }
const char *face_catalogue_id(int i) { return (i >= 0 && i < s_count) ? s_chars[i].id : NULL; }

const face_character_t *face_catalogue_find(const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < s_count; i++)
        if (!strcmp(s_chars[i].id, id)) return &s_chars[i];
    return NULL;
}
