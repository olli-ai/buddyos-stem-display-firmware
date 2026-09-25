/*
 * The face renderer: catalogue sprites composited by LVGL, moved by the interpreter.
 *
 * Layer 0 ("face") is the whole head. It is a container holding the background and
 * the five feature images, so a nod moves everything at once — the commonest motion
 * there is, and the one a child cannot build by keeping five features in sync.
 */
#include "face_view.h"
#include "face_motion.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "face";

static lv_obj_t *s_root;                      /* the "face" layer */
static lv_obj_t *s_layer[FACE_LAYER_COUNT];   /* 1..5; [0] is s_root */
static int s_state;
static int16_t s_shown[FACE_LAYER_COUNT];     /* which sprite each layer is showing */

/* LAYERS order in motion.ts: face, browL, eyeL, browR, eyeR, mouth.
 * face_design rows are stored browL, eyeL, browR, eyeR, mouth. */
static const int DESIGN_TO_LAYER[5] = { 1, 2, 3, 4, 5 };

static void show_sprite(int layer, int16_t idx)
{
    if (idx < 0 || idx >= face_sprite_count || s_shown[layer] == idx) return;
    const face_sprite_t *sp = &face_sprites[idx];
    s_shown[layer] = idx;
    if (!sp->img) { lv_obj_add_flag(s_layer[layer], LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(s_layer[layer], LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(s_layer[layer], sp->img);
    lv_obj_set_pos(s_layer[layer], sp->x, sp->y);
}

void face_view_create(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, face_bg.header.w, face_bg.header.h);
    lv_obj_center(s_root);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bg = lv_image_create(s_root);
    lv_image_set_src(bg, &face_bg);
    lv_obj_set_pos(bg, 0, 0);

    s_layer[0] = s_root;
    for (int i = 1; i < FACE_LAYER_COUNT; i++) {
        s_layer[i] = lv_image_create(s_root);
        s_shown[i] = -1;
        /* Transforms turn about the sprite's own middle: a brow that rotates about the
         * corner of the screen is not a raised brow, it is a catastrophe. */
        lv_image_set_pivot(s_layer[i], 0, 0);
        lv_obj_add_flag(s_layer[i], LV_OBJ_FLAG_HIDDEN);
    }
    face_view_set_state(0);
}

void face_view_set_state(int state)
{
    if (state < 0 || state >= face_state_count) return;
    s_state = state;
    for (int i = 0; i < 5; i++) show_sprite(DESIGN_TO_LAYER[i], face_design[state][i].sprite);
    ESP_LOGI(TAG, "state -> %s (%u ms loop)", face_states[state].name, face_states[state].duration);
}

void face_view_tick(uint32_t elapsed_ms)
{
    face_values_t v[FACE_LAYER_COUNT];
    face_motion_sample(s_state, elapsed_ms, v);

    /* The head. LVGL style transforms work on any object, so the container carries
     * the whole face without the background needing to know anything about it. */
    lv_obj_set_style_translate_x(s_root, (int32_t)(v[0].x * face_bg.header.w / 100), 0);
    lv_obj_set_style_translate_y(s_root, (int32_t)(v[0].y * face_bg.header.h / 100), 0);
    lv_obj_set_style_transform_scale_x(s_root, (int32_t)(v[0].scale * 256.0f), 0);
    lv_obj_set_style_transform_scale_y(s_root, (int32_t)(v[0].scale * v[0].scale_y * 256.0f), 0);
    lv_obj_set_style_transform_rotation(s_root, (int32_t)(v[0].rotation * 10.0f), 0);
    lv_obj_set_style_opa(s_root, (lv_opa_t)(v[0].opacity * 255.0f), 0);

    for (int i = 1; i < FACE_LAYER_COUNT; i++) {
        if (v[i].sprite >= 0) show_sprite(i, v[i].sprite);
        if (lv_obj_has_flag(s_layer[i], LV_OBJ_FLAG_HIDDEN)) continue;
        const face_sprite_t *sp = &face_sprites[s_shown[i]];
        /* x/y are in face units (the 0..100 the parts are drawn in), so a value means
         * the same thing here as it does at 54 px on a device card in the browser. */
        lv_obj_set_pos(s_layer[i],
                       sp->x + (int32_t)(v[i].x * face_bg.header.w / 100),
                       sp->y + (int32_t)(v[i].y * face_bg.header.h / 100));
        lv_image_set_scale_x(s_layer[i], (int32_t)(v[i].scale * 256.0f));
        lv_image_set_scale_y(s_layer[i], (int32_t)(v[i].scale * v[i].scale_y * 256.0f));
        lv_image_set_rotation(s_layer[i], (int32_t)(v[i].rotation * 10.0f));
        lv_obj_set_style_opa(s_layer[i], (lv_opa_t)(v[i].opacity * 255.0f), 0);
    }
}
