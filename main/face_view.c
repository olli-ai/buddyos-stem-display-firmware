/*
 * The face renderer: catalogue sprites composited by LVGL, moved by the interpreter.
 *
 * The "face" layer is the whole head — a nod, a bob, a shake — and the obvious way to
 * build it is a container with a transform on it, letting LVGL move the children.
 * That was the first version, and the simulator killed it: transforming a container
 * makes LVGL render it through an intermediate *layer*, and a 240x240 layer is
 * 115 KB at RGB565 or 230 KB at ARGB8888. With LVGL's default 64 KB heap the
 * allocation simply fails and the refresh spins forever; giving it 4 MB fixed it on a
 * PC. An ESP32-S3 has ~320 KB of internal SRAM in total, and the planning docs
 * already record BLE dying at 1,351 B free, so buying a frame-sized layer to nod a
 * head is not a trade this device can make.
 *
 * So the head is composed arithmetically instead. Every part — the background and the
 * five features — is an lv_image, and an lv_image transforms itself without any
 * layer. Each one is given the face transform composed with its own, and placed so
 * that its *centre* lands where the composition puts it. Because a rotation or a
 * scale about an image's own centre leaves that centre fixed, positioning the centre
 * is exact rather than an approximation.
 */
#include "face_view.h"
#include "face_motion.h"
#include "face_catalogue.h"
#include <math.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "face";

static lv_obj_t *s_root;                      /* a plain container: never transformed */
static lv_obj_t *s_bg;
static lv_obj_t *s_layer[FACE_LAYER_COUNT];   /* 1..5; [0] is the whole head */
static int s_state;
static int16_t s_shown[FACE_LAYER_COUNT];
/* What each layer is actually drawing, resolved once at swap time: a baked sprite or
 * a character's own artwork, which do not live in the same array. Keeping the
 * resolved pointer means the render path never asks again. */
static const lv_image_dsc_t *s_img[FACE_LAYER_COUNT];
static int16_t s_ix[FACE_LAYER_COUNT], s_iy[FACE_LAYER_COUNT];
/* The character whose body is showing, or NULL for the one baked into the firmware. */
static const face_character_t *s_char;
/* Requested from the MQTT task, applied on the render tick. LVGL is not thread safe
 * and this is the third thing to learn it the same way: the first version swapped the
 * background image here and the apply loop died mid-design, silently. */
static const face_character_t * volatile s_want_char;
static volatile bool s_char_pending;
static volatile uint32_t s_want_rgb;
static volatile bool s_want_original = true, s_color_pending;
static int16_t s_live_mouth = -1;             /* lip-sync, or -1 for the authored mouth */

/* The design in force: the baked one until a directive replaces it. Kept as sprite
 * indices rather than part ids because that is what drawing needs, and resolving
 * names once at the door means the render path never does string work. */
static int16_t s_parts[8][5];
static bool s_parts_set;
/* Set by whoever received a design, cleared by the render tick. LVGL is not thread
 * safe, and a design arrives on the MQTT task: touching sprites from there stopped
 * the apply loop dead after its first state, without a crash to explain why. So the
 * only thing that crosses tasks is this flag, and the drawing happens where all the
 * other drawing happens. */
static volatile bool s_parts_dirty;
/* A state asked for from somewhere that is not the render task — a directive, and
 * later the voice pipeline. Same rule as the design: only the request crosses, the
 * drawing happens where the drawing happens. */
static volatile int s_want_state = -1;
static volatile bool s_driven;

#define MOUTH_LAYER 5

/* face_design rows are browL, eyeL, browR, eyeR, mouth — the LAYERS order minus
 * the head itself. */
static const int DESIGN_TO_LAYER[5] = { 1, 2, 3, 4, 5 };

/** What state `st` draws now — the design received, or the one baked in. */
static int16_t part_of(int st, int i)
{
    return s_parts_set ? s_parts[st][i] : face_design[st][i].sprite;
}

/* browL, eyeL, browR, eyeR, mouth -> the character's own artwork slot, or -1.
 * A character's artist drew eyes and a mouth, never brows. */
static const int ART_SLOT[FACE_LAYER_COUNT] = { -1, -1, 0, -1, 1, 2 };

static void show_sprite(int layer, int16_t idx)
{
    if (s_shown[layer] == idx) return;
    const lv_image_dsc_t *img = NULL;
    int16_t x = 0, y = 0;

    if (idx == FACE_PART_ARTWORK) {
        /* The face this character was drawn with. Only the catalogue has it — the
         * built-in fallback is a plain drawn eye, which is what a kit shows when the
         * partition is empty. */
        int slot = ART_SLOT[layer];
        if (s_char && slot >= 0 && s_char->art[slot].header.w) {
            img = &s_char->art[slot];
            x = s_char->art_x[slot];
            y = s_char->art_y[slot];
        }
    } else if (idx >= 0 && idx < face_sprite_count) {
        const face_sprite_t *sp = &face_sprites[idx];
        img = sp->img; x = sp->x; y = sp->y;
    } else {
        return;
    }

    s_shown[layer] = idx;
    s_img[layer] = img; s_ix[layer] = x; s_iy[layer] = y;
    if (!img) { lv_obj_add_flag(s_layer[layer], LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(s_layer[layer], LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(s_layer[layer], img);
}

void face_view_set_character(const face_character_t *c)
{
    s_want_char = c;
    s_char_pending = true;
}

/** On the render task: actually wear it. */
static void wear_character(const face_character_t *c)
{
    s_char = c;
    lv_image_set_src(s_bg, c ? &c->bg : &face_bg);
    /* Every layer showing artwork is now showing somebody else's, so make them all
     * resolve again rather than trusting what is on screen. */
    for (int i = 1; i < FACE_LAYER_COUNT; i++) s_shown[i] = -1000;
    s_parts_dirty = true;
}

void face_view_create(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, face_bg.header.w, face_bg.header.h);
    lv_obj_center(s_root);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    s_bg = lv_image_create(s_root);
    lv_image_set_src(s_bg, &face_bg);

    for (int i = 1; i < FACE_LAYER_COUNT; i++) {
        s_layer[i] = lv_image_create(s_root);
        s_shown[i] = -1000;
        lv_obj_add_flag(s_layer[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int st = 0; st < face_state_count && st < 8; st++)
        for (int i = 0; i < 5; i++) s_parts[st][i] = face_design[st][i].sprite;
    face_view_set_state(0);
}

int face_view_state(void) { return s_state; }

void face_view_set_body_color(uint32_t rgb, bool original)
{
    s_want_rgb = rgb;
    s_want_original = original;
    s_color_pending = true;
}

/** On the render task. `original` is the mascot as its artist shaded it; any other
 *  colour fills the silhouette flat, which is exactly what the website does — it
 *  masks the artwork and paints one colour through it — so the two agree by
 *  construction rather than because anyone matched them by eye. */
static void paint_body(uint32_t rgb, bool original)
{
    if (!s_bg) return;
    lv_obj_set_style_image_recolor(s_bg, lv_color_hex(rgb), 0);
    lv_obj_set_style_image_recolor_opa(s_bg, original ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
}

void face_view_request_state(int state)
{
    if (state < 0 || state >= face_state_count) return;
    s_want_state = state;
    s_driven = true;
}

/** True once something outside has taken charge of which face is shown. */
bool face_view_is_driven(void) { return s_driven; }

void face_view_reset_parts(void)
{
    for (int st = 0; st < face_state_count && st < 8; st++)
        for (int i = 0; i < 5; i++) s_parts[st][i] = face_design[st][i].sprite;
    s_parts_dirty = true;
}

void face_view_set_state_parts(int state, const int16_t parts[5])
{
    if (state < 0 || state >= face_state_count || state >= 8) return;
    for (int i = 0; i < 5; i++) s_parts[state][i] = parts[i];
    s_parts_set = true;
    /* Redrawn on the next tick rather than here. A child watching should still see
     * it happen — the next tick is 16 ms away. */
    if (state == s_state) s_parts_dirty = true;
}

void face_view_set_live_mouth(int16_t sprite)
{
    s_live_mouth = sprite;
}

void face_view_set_state(int state)
{
    if (state < 0 || state >= face_state_count) return;
    s_state = state;
    for (int i = 0; i < 5; i++) show_sprite(DESIGN_TO_LAYER[i], part_of(state, i));
    ESP_LOGI(TAG, "state -> %s (%u ms loop)", face_states[state].name, face_states[state].duration);
}

/* Place one image under the face transform composed with its own.
 * `ox, oy` is the sprite's untransformed top-left; w, h its size. */
static void place(lv_obj_t *obj, const face_values_t *head, const face_values_t *own,
                  int32_t ox, int32_t oy, int32_t w, int32_t h)
{
    const float FCX = face_bg.header.w / 2.0f, FCY = face_bg.header.h / 2.0f;
    const float UNIT = face_bg.header.w / 100.0f;   /* face units -> pixels */

    /* Where this sprite's centre sits before the head moves. */
    float cx = ox + w / 2.0f + own->x * UNIT;
    float cy = oy + h / 2.0f + own->y * UNIT;

    /* Then the head: rotate and scale that centre about the face's middle. */
    float a = head->rotation * (float)M_PI / 180.0f;
    float sx = head->scale, sy = head->scale * head->scale_y;
    float dx = cx - FCX, dy = cy - FCY;
    float rx = FCX + (dx * cosf(a) - dy * sinf(a)) * sx + head->x * UNIT;
    float ry = FCY + (dx * sinf(a) + dy * cosf(a)) * sy + head->y * UNIT;

    /* A transform about the image's own centre leaves that centre where it is, so
     * putting the centre in the right place is all the positioning needed. */
    lv_image_set_pivot(obj, w / 2, h / 2);
    lv_image_set_scale_x(obj, (int32_t)(sx * own->scale * 256.0f));
    lv_image_set_scale_y(obj, (int32_t)(sy * own->scale * own->scale_y * 256.0f));
    lv_image_set_rotation(obj, (int32_t)((head->rotation + own->rotation) * 10.0f));
    lv_obj_set_pos(obj, (int32_t)lroundf(rx - w / 2.0f), (int32_t)lroundf(ry - h / 2.0f));
    lv_obj_set_style_opa(obj, (lv_opa_t)(head->opacity * own->opacity * 255.0f), 0);
}

void face_view_tick(uint32_t elapsed_ms)
{
    /* Everything a design asks for arrives on some other task and is applied here.
     * Three separate bugs came from calling LVGL where the message was parsed, each
     * of which killed the parser silently mid-design; nothing in this file may be
     * called from anywhere but this tick. */
    if (s_char_pending) {
        s_char_pending = false;
        wear_character((const face_character_t *)s_want_char);
    }
    if (s_color_pending) {
        s_color_pending = false;
        paint_body(s_want_rgb, s_want_original);
    }
    int want = s_want_state;
    if (want >= 0) {
        s_want_state = -1;
        if (want != s_state) face_view_set_state(want);
    }
    if (s_parts_dirty) {
        s_parts_dirty = false;
        for (int i = 0; i < 5; i++) show_sprite(DESIGN_TO_LAYER[i], part_of(s_state, i));
    }

    face_values_t v[FACE_LAYER_COUNT];
    face_motion_sample(s_state, elapsed_ms, v);

    static const face_values_t REST = { 0, 0, 1, 1, 0, 1, -1 };
    place(s_bg, &v[0], &REST, 0, 0, face_bg.header.w, face_bg.header.h);

    for (int i = 1; i < FACE_LAYER_COUNT; i++) {
        if (i == MOUTH_LAYER && s_live_mouth >= 0) {
            /* Live speech wins over whatever the mouth track was doing. */
            show_sprite(i, s_live_mouth);
        } else if (i == MOUTH_LAYER && v[i].sprite < 0) {
            /* Released, and this state has no mouth track to fall back to: put the
             * designed mouth back rather than leaving the last spoken shape on. */
            show_sprite(i, part_of(s_state, 4));
        } else if (v[i].sprite >= 0) {
            show_sprite(i, v[i].sprite);
        }
        if (s_shown[i] < 0 || lv_obj_has_flag(s_layer[i], LV_OBJ_FLAG_HIDDEN)) continue;
        if (!s_img[i]) continue;
        place(s_layer[i], &v[0], &v[i], s_ix[i], s_iy[i], s_img[i]->header.w, s_img[i]->header.h);
    }
}
