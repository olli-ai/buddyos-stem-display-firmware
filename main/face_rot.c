/* The glass's angle, stored on the kit. See face_rot.h. */
#include "face_rot.h"
#include "board_pins.h"
#include "nvs.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdbool.h>

static const char *TAG = "face-rot";

/* The same namespace the volume uses: one place for what this kit was told about
 * itself, as opposed to what the gateway knows about it. */
#define ROT_NS   "buddy"
#define ROT_KEY  "face_rot"

static int s_deg = -1000;               /* unread */

/* The P4 turns its body with the PPA, which cannot rotate while it scales, so the
 * face there is never turned and the board is built on that. Refusing here rather
 * than at the call sites keeps the rule in one place: ask this board for an angle and
 * it answers 0, whatever is stored. */
#if CONFIG_IDF_TARGET_ESP32P4
#define ROT_SUPPORTED 0
#else
#define ROT_SUPPORTED 1
#endif

/** -180..180, so that 350 and -10 are the same small turn and arithmetic stays sane. */
static int normalise(int deg)
{
    deg %= 360;
    if (deg > 180) deg -= 360;
    if (deg < -180) deg += 360;
    return deg;
}

int face_rot_deg(void)
{
#if !ROT_SUPPORTED
    return 0;
#else
    if (s_deg != -1000) return s_deg;
    int32_t v = BOARD_FACE_ROT_DEG;
    nvs_handle_t h;
    if (nvs_open(ROT_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, ROT_KEY, &v) != ESP_OK) v = BOARD_FACE_ROT_DEG;
        nvs_close(h);
    }
    s_deg = normalise((int)v);
    ESP_LOGI(TAG, "the glass is turned %d deg%s", s_deg,
             (int32_t)s_deg == (int32_t)BOARD_FACE_ROT_DEG ? " (this board's default)" : "");
    return s_deg;
#endif
}

bool face_rot_store(int deg)
{
#if !ROT_SUPPORTED
    (void)deg;
    ESP_LOGW(TAG, "this board cannot turn its face; ignoring");
    return false;
#else
    deg = normalise(deg);
    nvs_handle_t h;
    if (nvs_open(ROT_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_i32(h, ROT_KEY, deg) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) ESP_LOGI(TAG, "the glass will be %d deg after a restart", deg);
    return ok;
#endif
}
