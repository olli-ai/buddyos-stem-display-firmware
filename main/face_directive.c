#include "face_directive.h"
#include "face_view.h"
#include "face_lipsync.h"
#include "generated/face_assets.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "design";

#define NVS_NS   "buddy"
#define NVS_KEY  "design"
/* One design is a few hundred bytes of choices; a generous ceiling still costs
 * nothing, and refusing a larger one is better than truncating it into nonsense. */
#define DESIGN_MAX 4096

/** The catalogue lookup: a part id from the website to a sprite baked here.
 *  Linear over ~86 entries, run a handful of times per design — measuring it would
 *  cost more than it saves. */
static int16_t sprite_of(const char *layer, const char *part)
{
    if (!part || !*part) return -1;
    for (int i = 0; i < face_sprite_count; i++)
        if (!strcmp(face_sprites[i].layer, layer) && !strcmp(face_sprites[i].part, part))
            return (int16_t)i;
    return -1;
}

/** ADR 0009's fallback, out loud. */
static int16_t resolve(const char *layer, const char *part, int16_t fallback, const char *state)
{
    int16_t s = sprite_of(layer, part);
    if (s >= 0) return s;
    if (part && *part)
        ESP_LOGW(TAG, "%s/%s: no '%s' in this catalogue, keeping what was there",
                 state, layer, part);
    return fallback;
}

/** How the mouth moves while talking: a style, then any per-sound override. */
static void apply_talk(const cJSON *talk)
{
    if (!cJSON_IsObject(talk)) return;
    int16_t mouths[8];
    memcpy(mouths, face_viseme_sprite, sizeof(mouths));

    const cJSON *style = cJSON_GetObjectItem(talk, "style");
    if (cJSON_IsString(style)) {
        for (int i = 0; i < face_mouth_style_count; i++)
            if (!strcmp(face_mouth_styles[i].id, style->valuestring)) {
                memcpy(mouths, face_mouth_styles[i].sprite, sizeof(mouths));
                ESP_LOGI(TAG, "talks in the '%s' style", style->valuestring);
                break;
            }
    }

    /* Per-sound overrides ride on top of the style, exactly as the picker presents
     * them: choose a way of talking, then change the one sound you did not like. */
    static const char *NAMES[8] = { "rest", "mbp", "small", "mid", "wide", "round", "pucker", "teeth" };
    const cJSON *parts = cJSON_GetObjectItem(talk, "parts");
    if (cJSON_IsObject(parts))
        for (int v = 0; v < 8; v++) {
            const cJSON *p = cJSON_GetObjectItem(parts, NAMES[v]);
            if (!cJSON_IsString(p)) continue;
            int16_t s = sprite_of("mouth", p->valuestring);
            if (s >= 0) mouths[v] = s;
            else ESP_LOGW(TAG, "talk/%s: no mouth '%s' here", NAMES[v], p->valuestring);
        }

    face_lipsync_set_mouths(mouths);
}

bool face_directive_apply(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) { ESP_LOGE(TAG, "not JSON, ignoring %u B", (unsigned)len); return false; }

    /* Accept either the directive's `data` or the design's `extra` straight from
     * the gateway — they are the same object, and which one arrives depends on who
     * is publishing. Being strict here would only make a bench test harder. */
    const cJSON *extra = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsObject(extra)) extra = root;

    const cJSON *faces = cJSON_GetObjectItem(extra, "faces");
    const cJSON *states = faces ? cJSON_GetObjectItem(faces, "states") : NULL;
    bool changed = false;

    if (cJSON_IsObject(states)) {
        /* The website authors seven states; this device shows four. The other three
         * (asleep, busy, error) have nowhere to appear yet, so they are skipped
         * rather than mapped onto something they are not. */
        for (int st = 0; st < face_state_count; st++) {
            const cJSON *s = cJSON_GetObjectItem(states, face_states[st].name);
            if (!cJSON_IsObject(s)) continue;
            const cJSON *eyes = cJSON_GetObjectItem(s, "eyes");
            const cJSON *mouth = cJSON_GetObjectItem(s, "mouth");
            const cJSON *brows = cJSON_GetObjectItem(s, "brows");
            const char *name = face_states[st].name;
            int16_t parts[5] = {
                resolve("browL", cJSON_GetStringValue(brows), face_design[st][0].sprite, name),
                resolve("eyeL",  cJSON_GetStringValue(eyes),  face_design[st][1].sprite, name),
                resolve("browR", cJSON_GetStringValue(brows), face_design[st][2].sprite, name),
                resolve("eyeR",  cJSON_GetStringValue(eyes),  face_design[st][3].sprite, name),
                resolve("mouth", cJSON_GetStringValue(mouth), face_design[st][4].sprite, name),
            };
            face_view_set_state_parts(st, parts);
            ESP_LOGI(TAG, "%s: eyes=%s mouth=%s brows=%s", name,
                     cJSON_GetStringValue(eyes)  ? cJSON_GetStringValue(eyes)  : "-",
                     cJSON_GetStringValue(mouth) ? cJSON_GetStringValue(mouth) : "-",
                     cJSON_GetStringValue(brows) ? cJSON_GetStringValue(brows) : "-");
            changed = true;
        }
    }

    apply_talk(cJSON_GetObjectItem(extra, "talk"));

    /* Motion is not applied yet: the keyframe tracks are baked, and reading them
     * from a design means an interpreter that builds tracks at runtime. Saying so
     * beats a face that silently keeps the factory's blink. */
    if (cJSON_GetObjectItem(extra, "motion"))
        ESP_LOGW(TAG, "this design has motion; the device still plays the built-in loops");

    cJSON_Delete(root);
    return changed;
}

/** Keep it, so the face survives a power cut. Stored raw: the thing that arrived is
 *  the thing that gets replayed, and no second format can drift from the first. */
static void remember(const char *json, size_t len)
{
    if (len > DESIGN_MAX) { ESP_LOGW(TAG, "design too large to keep (%u B)", (unsigned)len); return; }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err)); return; }
    err = nvs_set_blob(h, NVS_KEY, json, len);
    if (err == ESP_OK) err = nvs_commit(h);
    /* Reported, not swallowed. A design that applies but is not kept looks perfect
     * until the power blinks, and then looks like the face was never sent. */
    if (err != ESP_OK) ESP_LOGE(TAG, "could not keep the design: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "kept this design (%u B)", (unsigned)len);
    nvs_close(h);
}

bool face_directive_receive(const char *json, size_t len)
{
    bool ok = face_directive_apply(json, len);
    if (ok) remember(json, len);
    return ok;
}

void face_directive_restore(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_blob(h, NVS_KEY, NULL, &len) == ESP_OK && len && len <= DESIGN_MAX) {
        char *buf = malloc(len);
        if (buf && nvs_get_blob(h, NVS_KEY, buf, &len) == ESP_OK) {
            ESP_LOGI(TAG, "restoring the design this Buddy was last given (%u B)", (unsigned)len);
            face_directive_apply(buf, len);
        }
        free(buf);
    }
    nvs_close(h);
}
