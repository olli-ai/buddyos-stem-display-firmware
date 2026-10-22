#include "face_directive.h"
#include "face_view.h"
#include "face_motion.h"
#include "face_catalogue.h"
#include "face_lipsync.h"
#include "generated/face_assets.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG = "design";

static void forget(void);

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
    /* "artwork" is not in the sprite array: it is whatever this character's artist
     * drew, which the catalogue carries per character. */
    if (!strcmp(part, "artwork")) return FACE_PART_ARTWORK;
    for (int i = 0; i < face_sprite_count; i++)
        if (!strcmp(face_sprites[i].layer, layer) && !strcmp(face_sprites[i].part, part))
            return (int16_t)i;
    return -1;
}

/* ── a design's own motion ────────────────────────────────────────────────────
 *
 * The website authors tracks of exactly the shape the baker produces, so this is a
 * conversion rather than an interpreter: names to indices, floats to the baked
 * fixed-point units, and the keys handed to face_motion.c to sample like any other.
 *
 * What the hardware can do with them differs by property, and that is worth knowing
 * before reading a bug report. Every layer's transform is LVGL's, so brows, eyes and
 * the mouth take all of it. The *body* is composited by the PPA (face_view.c), which
 * translates to the pixel but cannot rotate by less than a quarter turn or scale by
 * less than a sixteenth — so on a whole-head track, `x` and `y` move the body and
 * `rotation` and `scale` move only the features over it. `shake` is a rotation
 * track, so it shakes the face without shaking the head.
 */

/* The website's lists, in its order: these indices are the wire format. */
static const char *MOTION_LAYERS[] = { "face", "browL", "eyeL", "browR", "eyeR", "mouth" };
static const char *MOTION_PROPS[]  = { "x", "y", "scale", "scaleY", "rotation", "opacity" };
static const char *MOTION_EASINGS[] = { "step", "linear", "easeIn", "easeOut",
                                        "easeInOut", "back", "bounce" };

static int index_of(const char *const *list, int n, const char *s)
{
    if (!s) return -1;
    for (int i = 0; i < n; i++) if (!strcmp(list[i], s)) return i;
    return -1;
}
#define N_OF(a) ((int)(sizeof(a) / sizeof(*(a))))

/* One numeric value into the units the baker uses. Clamped rather than wrapped: a
 * design is network input, and an int16 that rolls over draws a face inside out. */
static int16_t fixed(uint8_t prop, double v)
{
    double scaled;
    switch (prop) {
        case 0: case 1: scaled = v * 16.0;    break;   /* x, y: face units */
        case 2: case 3: scaled = v * 1024.0;  break;   /* scale, scaleY */
        case 4:         scaled = v * 64.0;    break;   /* rotation, degrees */
        default:        scaled = v * 255.0;   break;   /* opacity */
    }
    if (scaled > 32767.0) scaled = 32767.0;
    if (scaled < -32768.0) scaled = -32768.0;
    return (int16_t)lround(scaled);
}

/* `extra.motion.states` — the child's own loops, replacing the baked ones per state. */
static void apply_motion(const cJSON *motion)
{
    face_motion_design_clear();
    if (!motion) return;                 /* no motion authored: the baked loops, as always */
    const cJSON *states = cJSON_IsObject(motion) ? cJSON_GetObjectItem(motion, "states") : NULL;
    if (!cJSON_IsObject(states)) {
        ESP_LOGW(TAG, "this design has a motion key with no states in it");
        return;
    }

    int added = 0, refused = 0;
    for (int st = 0; st < face_state_count; st++) {
        const cJSON *sm = cJSON_GetObjectItem(states, face_states[st].name);
        if (!cJSON_IsObject(sm)) continue;
        const cJSON *dur = cJSON_GetObjectItem(sm, "duration");
        const cJSON *tracks = cJSON_GetObjectItem(sm, "tracks");
        if (!cJSON_IsNumber(dur) || dur->valuedouble <= 0 || !cJSON_IsArray(tracks)) continue;
        uint16_t duration = (uint16_t)(dur->valuedouble > 65535 ? 65535 : dur->valuedouble);

        const cJSON *tr;
        cJSON_ArrayForEach(tr, tracks) {
            if (!cJSON_IsObject(tr)) continue;
            int layer = index_of(MOTION_LAYERS, N_OF(MOTION_LAYERS),
                                 cJSON_GetStringValue(cJSON_GetObjectItem(tr, "layer")));
            const char *pname = cJSON_GetStringValue(cJSON_GetObjectItem(tr, "prop"));
            const cJSON *keys = cJSON_GetObjectItem(tr, "keys");
            if (layer < 0 || !pname || !cJSON_IsArray(keys)) { refused++; continue; }
            bool is_sprite = !strcmp(pname, "sprite");
            int prop = is_sprite ? 255 : index_of(MOTION_PROPS, N_OF(MOTION_PROPS), pname);
            if (prop < 0) { refused++; continue; }

            face_key_t buf[48];
            int n = 0;
            const cJSON *k;
            cJSON_ArrayForEach(k, keys) {
                if (n >= (int)(sizeof buf / sizeof *buf) || !cJSON_IsObject(k)) break;
                const cJSON *kt = cJSON_GetObjectItem(k, "t");
                if (!cJSON_IsNumber(kt)) continue;
                int16_t v;
                if (is_sprite) {
                    /* A sprite key names a part, as the design's states do. Resolved
                     * against this layer, so `closed` means the closed *eye*. */
                    v = sprite_of(MOTION_LAYERS[layer],
                                  cJSON_GetStringValue(cJSON_GetObjectItem(k, "id")));
                    if (v < 0 && v != FACE_PART_ARTWORK) continue;
                } else {
                    const cJSON *kv = cJSON_GetObjectItem(k, "v");
                    if (!cJSON_IsNumber(kv)) continue;
                    v = fixed((uint8_t)prop, kv->valuedouble);
                }
                int e = index_of(MOTION_EASINGS, N_OF(MOTION_EASINGS),
                                 cJSON_GetStringValue(cJSON_GetObjectItem(k, "e")));
                buf[n++] = (face_key_t){
                    .t = (uint16_t)(kt->valuedouble > 65535 ? 65535 : kt->valuedouble),
                    .v = v,
                    .e = (uint8_t)(e < 0 ? 4 : e),      /* the website's own default */
                };
            }
            if (!n) { refused++; continue; }

            const cJSON *every = cJSON_GetObjectItem(tr, "every");
            uint16_t ev = cJSON_IsNumber(every) && every->valuedouble > 0
                        ? (uint16_t)(every->valuedouble > 65535 ? 65535 : every->valuedouble) : 0;
            if (face_motion_design_add(st, duration, (uint8_t)layer, (uint8_t)prop, buf, n, ev))
                added++;
            else
                refused++;
        }
    }
    if (added) {
        /* Which states the child re-authored, by name — the states they did not keep
         * the baked loop, and that distinction is invisible otherwise. */
        char named[96] = "";
        for (int st = 0; st < face_state_count; st++)
            if (face_motion_state_has_design(st)) {
                strlcat(named, named[0] ? "," : "", sizeof named);
                strlcat(named, face_states[st].name, sizeof named);
            }
        ESP_LOGI(TAG, "this design brings its own motion: %d tracks over %s%s",
                 added, named, refused ? " (some refused)" : "");
    } else if (refused) {
        ESP_LOGW(TAG, "this design's motion could not be read; the built-in loops stay");
    } else {
        ESP_LOGW(TAG, "this design's motion names no state this device has");
    }
}

/* ── expressions ──────────────────────────────────────────────────────────────
 *
 * The face presets from `PRESETS` in ../buddyos-stem-fe/src/data/faceParts.ts, which
 * is the source of truth for all three lists that have to agree: these triples, the
 * ids omni gives the model (`KIT_EXPRESSIONS` in
 * ../buddyos-omni/app/plugins/visual_face_emotion/face_context.py), and the parts
 * baked into face_sprites.
 *
 * Transcribed rather than generated, which is a debt — but not a silent one. The kit
 * advertises exactly these ids and omni refuses any it does not know, logging
 * `not a kit expression`, so a drift between the two shows up in a log line on the
 * first conversation rather than as a face that never changes.
 *
 * All eighteen. omni used to take the first ten and drop the rest where nobody could
 * see it happen; it no longer caps a kit at all, because a kit's vocabulary is a
 * closed set — every id has to be a key of KIT_EXPRESSIONS over there, so the list is
 * bounded by that table rather than by anything a device claims.
 *
 * `original` stays first because it is also the resting face, and the rest run from
 * the expressions a conversation reaches for most often to the ones it rarely does.
 * Order no longer decides what survives, but it is still the order the model reads.
 */
typedef struct { const char *id, *eyes, *brows, *mouth; } expression_t;

static const expression_t EXPRESSIONS[] = {
    { "original",  "artwork", "none",    "artwork"   },
    { "happy",     "sparkle", "none",    "grin"      },
    { "curious",   "wide",    "oneup",   "tinyu"     },
    { "surprised", "wide",    "raised",  "gasp"      },
    { "sad",       "tear",    "sad",     "frown"     },
    { "silly",     "wink",    "oneup",   "tongue"    },
    { "confused",  "lookup",  "oneup",   "squiggle"  },
    { "thinking",  "lookup",  "furrow",  "flatsmile" },
    { "love",      "heart",   "raised",  "smile"     },
    { "sleepy",    "closed",  "none",    "snore"     },
    { "neutral",   "round",   "none",    "flatsmile" },
    { "talking",   "round",   "none",    "open"      },
    { "focused",   "squint",  "furrow",  "neutral"   },
    { "cool",      "squint",  "flat",    "smirk"     },
    { "starry",    "star",    "raised",  "laugh"     },
    { "angry",     "angry",   "angry",   "bigfrown"  },
    { "dizzy",     "spiral",  "worried", "wave"      },
    { "ko",        "cross",   "worried", "squiggle"  },
};
#define N_EXPRESSIONS (sizeof EXPRESSIONS / sizeof *EXPRESSIONS)

size_t face_expressions_advertise(char *buf, size_t n)
{
    /* omni reads this as a `register_tools` control message and hands the contexts
     * to its plugins; the gateway passes it through, having first checked that a kit
     * is advertising nothing but `face2d`. */
    int w = snprintf(buf, n,
                     "{\"type\":\"register_tools\",\"client_tools\":[],"
                     "\"ui_context\":[{\"type\":\"face2d\",\"resting\":\"original\","
                     "\"emotions\":[");
    if (w < 0 || (size_t)w >= n) return 0;
    for (unsigned i = 0; i < N_EXPRESSIONS; i++) {
        int k = snprintf(buf + w, n - w, "%s\"%s\"", i ? "," : "", EXPRESSIONS[i].id);
        if (k < 0 || (size_t)(w + k) >= n) return 0;
        w += k;
    }
    int k = snprintf(buf + w, n - w, "]}]}");
    if (k < 0 || (size_t)(w + k) >= n) return 0;
    return (size_t)(w + k);
}

bool face_expression_wear(const char *id)
{
    if (!id) return false;
    for (unsigned i = 0; i < N_EXPRESSIONS; i++) {
        if (strcmp(EXPRESSIONS[i].id, id)) continue;
        const expression_t *e = &EXPRESSIONS[i];
        /* No fallback here, unlike a design: an expression this firmware advertised
         * is one whose every part it has, so a miss is a bug rather than a choice a
         * child made that cannot be drawn.
         *
         * The mouth is resolved and stored but not drawn — face_view's part_of()
         * leaves that layer to the design. Kept so the table stays the whole preset
         * and the policy lives in one place rather than two. */
        int16_t parts[5] = {
            sprite_of("browL", e->brows), sprite_of("eyeL", e->eyes),
            sprite_of("browR", e->brows), sprite_of("eyeR", e->eyes),
            sprite_of("mouth", e->mouth),
        };
        face_view_request_expression(parts);
        return true;
    }
    return false;
}

/** ADR 0009's fallback, out loud. */
static int16_t resolve(const char *layer, const char *part, int16_t fallback, const char *state)
{
    int16_t s = sprite_of(layer, part);
    /* FACE_PART_ARTWORK is a real answer, not a failure: it says "whatever this
     * character was drawn with". Treating every negative value as not-found made the
     * catalogue's whole reason for existing look like a missing part. */
    if (s >= 0 || s == FACE_PART_ARTWORK) return s;
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

/** Set when the design just applied was an empty one. Read once by `receive`. */
static bool s_cleared;
/** ...and set when what arrived was a design at all. A `face.state` directive is a
 *  passing mood, not something to keep: storing one overwrote the design a child had
 *  made, so the next power cut would have lost it and the log would have said
 *  "restoring the design this Buddy was last given" while restoring a mood. */
static bool s_is_design;

bool face_directive_apply(const char *json, size_t len)
{
    s_cleared = false;
    s_is_design = false;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) { ESP_LOGE(TAG, "not JSON, ignoring %u B", (unsigned)len); return false; }

    /* The directive topic carries more than faces — programs, and whatever comes
     * next — so anything that does not say `face.*` is somebody else's message and
     * is left alone. This is not fussiness: without it the device's own hello event
     * read as an empty design and erased the child's face, which is exactly the
     * shape of bug that would reach a classroom before it reached a test. */
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type) && strncmp(type->valuestring, "face.", 5) != 0) {
        cJSON_Delete(root);
        return false;
    }

    /* Which face to wear right now. A conversation moves through these faster than
     * anything redraws a design: listening while the child speaks, thinking while
     * the answer is found, speaking while it is said. The design says what each one
     * looks like; this says which one it is. */
    if (cJSON_IsString(type) && !strcmp(type->valuestring, "face.state")) {
        const cJSON *data = cJSON_GetObjectItem(root, "data");
        const cJSON *name = data ? cJSON_GetObjectItem(data, "state") : NULL;
        bool found = false;
        if (cJSON_IsString(name))
            for (int i = 0; i < face_state_count; i++)
                if (!strcmp(face_states[i].name, name->valuestring)) {
                    face_view_request_state(i);
                    ESP_LOGI(TAG, "showing '%s'", name->valuestring);
                    found = true;
                    break;
                }
        if (!found)
            ESP_LOGW(TAG, "no state '%s' here", cJSON_IsString(name) ? name->valuestring : "?");
        cJSON_Delete(root);
        return found;
    }

    /* Accept either the directive's `data` or the design's `extra` straight from
     * the gateway — they are the same object, and which one arrives depends on who
     * is publishing. Being strict here would only make a bench test harder. */
    const cJSON *extra = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsObject(extra)) extra = root;

    /* An empty design is not a malformed one: it is what the gateway sends when a
     * child deletes their design, and it means "back to the face you were born
     * with". Treating it as nothing to do would leave the deleted face on screen
     * forever, since the retained message is now this. */
    if (!cJSON_GetObjectItem(extra, "faces") && !cJSON_GetObjectItem(extra, "talk")
        && !cJSON_GetObjectItem(extra, "motion")) {
        ESP_LOGI(TAG, "design cleared — back to the built-in face");
        s_cleared = true;
        face_view_reset_parts();
        face_motion_design_clear();      /* back to the loops this kit was built with */
        face_lipsync_set_mouths(face_viseme_sprite);
        cJSON_Delete(root);
        forget();
        return true;
    }

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

    /* The look is the child's other half of the design — base, body colour, hair,
     * ears, accessories — and this firmware has artwork for exactly one of them. Say
     * so by name. A change that cannot be drawn and is not mentioned is the same
     * experience as a change that never arrived, which is the bug this replaces. */
    const cJSON *look = cJSON_GetObjectItem(extra, "look");
    if (cJSON_IsObject(look)) {
        const char *base = cJSON_GetStringValue(cJSON_GetObjectItem(look, "base"));
        const char *color = cJSON_GetStringValue(cJSON_GetObjectItem(look, "body_color"));
        if (base) {
            const face_character_t *c = face_catalogue_find(base);
            if (c) {
                face_view_set_character(c);
                ESP_LOGI(TAG, "this Buddy is a '%s'", base);
            } else if (strcmp(base, face_look_base)) {
                ESP_LOGW(TAG, "no '%s' in the catalogue; showing the built-in '%s'",
                         base, face_look_base);
                face_view_set_character(NULL);
            } else {
                face_view_set_character(NULL);
            }
        }
        if (color) {
            /* Applied, not merely reported: the body colour is the one part of the
             * look this firmware can honour, because tinting is an operation on the
             * pixels it already has rather than artwork it does not. */
            bool found = false;
            for (int i = 0; i < face_body_color_count; i++)
                if (!strcmp(face_body_colors[i].id, color)) {
                    bool original = !strcmp(color, "original");
                    face_view_set_body_color(face_body_colors[i].rgb, original);
                    ESP_LOGI(TAG, "body colour '%s'%s", color, original ? " (its own)" : "");
                    found = true;
                    break;
                }
            if (!found) ESP_LOGW(TAG, "no body colour '%s' in this catalogue", color);
        }
        static const char *EXTRAS[] = { "hair", "ears", "accessory", "cheeks" };
        for (unsigned i = 0; i < sizeof(EXTRAS) / sizeof(EXTRAS[0]); i++) {
            const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(look, EXTRAS[i]));
            if (v && strcmp(v, "none") && strcmp(v, "own"))
                ESP_LOGW(TAG, "%s '%s' is not in this firmware's artwork", EXTRAS[i], v);
        }
    }

    apply_talk(cJSON_GetObjectItem(extra, "talk"));

    apply_motion(cJSON_GetObjectItem(extra, "motion"));

    s_is_design = true;
    cJSON_Delete(root);
    return changed;
}

/** Drop what was kept, so a cleared design stays cleared across a reboot. */
static void forget(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_KEY);
    nvs_commit(h);
    nvs_close(h);
}

/** Keep it, so the face survives a power cut. Stored raw: the thing that arrived is
 *  the thing that gets replayed, and no second format can drift from the first. */
/* Is this the design the kit is already wearing?
 *
 * It matters because `face.update` is a *retained* MQTT message: the broker replays
 * it on every connect, so face_directive_receive() runs at each boot with exactly
 * what face_directive_restore() has just applied from NVS. Without this the kit
 * would parade all seven states every time it powered on, and the preview would
 * stop meaning "your design arrived" — which is the only thing it is for. */
static bool same_as_kept(const char *json, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t kept = 0;
    bool same = false;
    if (nvs_get_blob(h, NVS_KEY, NULL, &kept) == ESP_OK && kept == len && len) {
        char *buf = malloc(len);
        if (buf && nvs_get_blob(h, NVS_KEY, buf, &kept) == ESP_OK)
            same = memcmp(buf, json, len) == 0;
        free(buf);
    }
    nvs_close(h);
    return same;
}

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
    /* Asked before applying, because applying is what makes it the current one. */
    bool unchanged = same_as_kept(json, len);
    bool ok = face_directive_apply(json, len);
    /* Only an actual design is kept, and not one that was just cleared — storing
     * that would only put it straight back. */
    if (ok && s_is_design && !s_cleared) remember(json, len);
    /* And show it, if it is new. A design that *changes* is a child who just pressed
     * Save and is watching the kit; the same design arriving again is the broker
     * replaying a retained message, which happens at every boot. */
    if (ok && !s_cleared && !unchanged) face_view_preview_states();
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
