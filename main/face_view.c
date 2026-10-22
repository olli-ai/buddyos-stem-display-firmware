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
#include "esp_heap_caps.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_timer.h"
#include "face_ppa.h"
#include "face_demo_motion.h"
#endif
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
/* An expression the model chose mid-sentence, worn over the state's own parts. It
 * is a different axis from state: a Buddy can be `speaking` and `surprised` at once.
 * It covers the eyes and brows only — see part_of() for why the mouth stays the
 * child's.
 *
 * It is cleared at the start of the next *turn*, not at the next state change, and
 * that distinction was worth a measurement. The expression arrives while omni is
 * still `thinking` — it is sent as soon as the tag is reached, which is ahead of the
 * audio — so clearing on a state change wiped it a fraction of a second later when
 * the state became `speaking`, which is to say immediately before the words it was
 * chosen for. Observed on hardware: `face: sad` at 30522 ms, `state -> speaking` at
 * 31264 ms, and a face that never looked sad.
 *
 * Requested from the voice task, applied in the tick — see face_view_tick(). */
static int16_t s_expr[5];
static bool s_expr_on;
static volatile int16_t s_want_expr[5];
static volatile bool s_expr_pending, s_expr_clearing;

/* The Save preview. A design arrives and the kit wears each state in turn, because
 * the alternative is a child who never sees six of the seven faces they made: the
 * kit sits in `idle` between conversations, and `idle` is the one state whose
 * default (`original`) is the character's own artwork — designed to look untouched. */
#define PREVIEW_MS 1100
static volatile bool s_preview_req;
static int s_preview_i = -1;
static uint32_t s_preview_t0;
static int s_preview_back;

static volatile int s_want_state = -1;
static volatile bool s_driven;

#define MOUTH_LAYER 5

/* face_design rows are browL, eyeL, browR, eyeR, mouth — the LAYERS order minus
 * the head itself. */
static const int DESIGN_TO_LAYER[5] = { 1, 2, 3, 4, 5 };

/** What state `st` draws now — the design received, or the one baked in. */
/* Slot 4 of a parts array is the mouth; 0-3 are the brows and eyes. */
#define MOUTH_SLOT 4

static int16_t part_of(int st, int i)
{
    /* An expression takes the eyes and the brows, and leaves the mouth alone.
     *
     * The mouth is the part of a face a child actually designs — it is where `grin`
     * differs from `frown` — and letting the model overwrite it meant a designed
     * speaking face was replaced within a few seconds of the Buddy opening its
     * mouth, every time. Eyes and brows carry an expression perfectly well without
     * it: `surprised` is wide eyes and raised brows whatever the mouth is doing, and
     * while speech is playing the mouth belongs to lip-sync anyway.
     *
     * So the child owns the mouth, the model borrows the eyes. */
    if (s_expr_on && i != MOUTH_SLOT) return s_expr[i];
    return s_parts_set ? s_parts[st][i] : face_design[st][i].sprite;
}

/* What the face is actually made of right now, by sprite name — the only way to
 * tell "the design never arrived" from "the design arrived and something is drawing
 * over it", which read identically on a screen. */
static void report_worn(void);
static void report_worn(void)
{
    const char *n[5];
    for (int i = 0; i < 5; i++) {
        int16_t sp = part_of(s_state, i);
        n[i] = sp == FACE_PART_ARTWORK ? "artwork"
             : (sp >= 0 && sp < face_sprite_count) ? face_sprites[sp].part : "-";
    }
    ESP_LOGI(TAG, "wearing %s: brows=%s eyes=%s mouth=%s%s", face_states[s_state].name,
             n[0], n[1], n[4], s_expr_on ? "   <- expression over the design" : "");
}

void face_view_preview_states(void) { s_preview_req = true; }

void face_view_request_expression(const int16_t parts[5])
{
    if (parts) for (int i = 0; i < 5; i++) s_want_expr[i] = parts[i];
    s_expr_clearing = (parts == NULL);
    s_expr_pending = true;
}

/* browL, eyeL, browR, eyeR, mouth -> the character's own artwork slot, or -1.
 * A character's artist drew eyes and a mouth, never brows. */
static const int ART_SLOT[FACE_LAYER_COUNT] = { -1, -1, 0, -1, 1, 2 };

/* A catalogue character is baked smaller than the panel and scaled up when worn;
 * everything measured in its own space carries this factor. Declared here because
 * measure_features() below needs it. */
static float s_cat_scale = 1.0f;

/* Which feature each layer belongs to: face, browL, eyeL, browR, eyeR, mouth.
 * A brow belongs to its own eye, so it moves with it. */
static const int FEATURE_SLOT[FACE_LAYER_COUNT] = { -1, 0, 0, 1, 1, 2 };

/* The firmware's own face carries its eyes and mouth *inside* face_bg — that is why
 * `artwork` draws nothing extra on the built-in character. So there is no stored
 * anchor saying where its left eye is; the only record is where the drawn parts were
 * authored to sit, which is what these are for. One plain part per feature, measured
 * once, is the built-in face's anchor for that feature. */
static const struct { const char *layer, *part; } FEATURE_REF[3] = {
    { "eyeL", "round" }, { "eyeR", "round" }, { "mouth", "neutral" },
};

/* Where this character wants each feature, minus where the built-in face put it.
 * Added to every drawn part, so a designed eye lands on the character's eye rather
 * than on the eye of whichever face the sprites were drawn for. */
static int16_t s_feat_dx[3], s_feat_dy[3];

static int16_t find_sprite(const char *layer, const char *part)
{
    for (int i = 0; i < face_sprite_count; i++)
        if (!strcmp(face_sprites[i].layer, layer) && !strcmp(face_sprites[i].part, part))
            return (int16_t)i;
    return -1;
}

/* One delta per feature, not one per sprite: parts of a feature keep the sizes and
 * offsets their artist gave them relative to each other, and the whole feature moves
 * together. Recomputed whenever the character changes. */
static void measure_features(void)
{
    for (int slot = 0; slot < 3; slot++) {
        s_feat_dx[slot] = s_feat_dy[slot] = 0;
        if (!s_char || !s_char->art[slot].header.w) continue;   /* built-in: no shift */
        int16_t r = find_sprite(FEATURE_REF[slot].layer, FEATURE_REF[slot].part);
        if (r < 0 || !face_sprites[r].img) continue;
        const face_sprite_t *sp = &face_sprites[r];
        float cx = (s_char->art_x[slot] + s_char->art[slot].header.w / 2.0f) * s_cat_scale;
        float cy = (s_char->art_y[slot] + s_char->art[slot].header.h / 2.0f) * s_cat_scale;
        s_feat_dx[slot] = (int16_t)lroundf(cx - (sp->x + sp->img->header.w / 2.0f));
        s_feat_dy[slot] = (int16_t)lroundf(cy - (sp->y + sp->img->header.h / 2.0f));
    }
    if (s_char)
        ESP_LOGI(TAG, "'%s' wants its parts moved: eyeL %+d,%+d  eyeR %+d,%+d  mouth %+d,%+d",
                 s_char->id, s_feat_dx[0], s_feat_dy[0], s_feat_dx[1], s_feat_dy[1],
                 s_feat_dx[2], s_feat_dy[2]);
}

/* The panel over the catalogue's own size: see where it is set. */

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
            x = (int16_t)lroundf(s_char->art_x[slot] * s_cat_scale);
            y = (int16_t)lroundf(s_char->art_y[slot] * s_cat_scale);
        }
    } else if (idx >= 0 && idx < face_sprite_count) {
        /* A drawn part, authored for the built-in face. Shifted onto this character's
         * own feature, or it sits where somebody else's eye was — which on Pip put
         * the eyes low on the head with the mouth crowding between them. */
        const face_sprite_t *sp = &face_sprites[idx];
        img = sp->img; x = sp->x; y = sp->y;
        int slot = FEATURE_SLOT[layer];
        if (slot >= 0) { x += s_feat_dx[slot]; y += s_feat_dy[slot]; }
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
/* The body, flattened: one opaque RGB565 copy in PSRAM, with the alpha and the tint
 * already resolved into it.
 *
 * As shipped, the background is RGB565A8 and may carry a recolour, so every redraw
 * blended 518,400 pixels and recoloured them — twice the per-pixel work, sixty times
 * a second, for two things that only change when a child saves a design. Measured at
 * 720x720 a full redraw cost 75-141 ms; an opaque image is a copy, and a copy is what
 * lets the whole face move.
 *
 * Composited over black because that is what the screen behind it is (main.c sets
 * it), so the mascot's soft edge stays soft rather than turning into a cut-out.
 * Rebuilt on a character change or a colour change, which is to say rarely. */
/* A catalogue baked for a smaller face than this panel. The partition is 1.5 MB and
 * a 720x720 background is 1,519 KB on its own, so three characters only fit at 384 —
 * and then everything the catalogue carries is in 384 space while the firmware's own
 * sprites are in the panel's. Without this a character drew at its natural size in
 * the corner. The background is resampled once, here; the artwork parts are small
 * enough to let place() scale them per frame. */

static lv_image_dsc_t s_flat;
static uint16_t *s_flat_px;
#if CONFIG_IDF_TARGET_ESP32P4
/* The body is no longer an LVGL layer on this board; the PPA puts it on the panel
 * directly, under whatever LVGL draws afterwards. s_bx/s_by is where it sits this
 * frame.
 *
 * It goes down on *every* render, not only when it moves. The body is what erases
 * the previous frame — the screen's own background no longer does, or it would
 * paint over the body — so a render that skipped it would leave the last frame's
 * eyes and mouth behind and the face would double. That is also why the display
 * renders whole screens (see main.c). */
static int s_bx, s_by;
static uint32_t s_body_bg;
static int64_t s_render_t0;
#endif
static uint32_t s_flat_rgb;
static bool s_flat_original, s_flat_valid;

static void flatten(const lv_image_dsc_t *src, uint32_t rgb, bool original)
{
    const uint32_t sw = src->header.w, sh = src->header.h;
    const uint32_t dw = face_bg.header.w, dh = face_bg.header.h;   /* always the panel */
    const uint32_t sn = sw * sh, dn = dw * dh;
    if (!sw || !sh) return;
    if (!s_flat_px) {
        /* Cache-line aligned, because the PPA reads this buffer directly. */
        s_flat_px = heap_caps_aligned_alloc(FACE_PPA_ALIGN, FACE_PPA_ROUND(dn * 2),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_flat_px) { ESP_LOGW(TAG, "no PSRAM to flatten the body; drawing it blended"); return; }
    }
    /* RGB565A8 is planar: the whole colour plane, then the whole alpha plane. */
    const uint8_t *col = src->data, *alpha = src->data + sn * 2;
    const uint16_t tint = (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
    /* 16.16 steps, so a catalogue baked for a smaller face still fills the panel.
     * Bilinear, not nearest: this runs once per character, and 384 -> 720 by nearest
     * is visibly blocky on art that was vector a moment ago. */
    const uint32_t stepx = (sw << 16) / dw, stepy = (sh << 16) / dh;
    for (uint32_t dy = 0; dy < dh; dy++) {
        uint32_t fy = dy * stepy, y0 = fy >> 16, wy = (fy >> 8) & 0xFF;
        uint32_t y1 = y0 + 1 < sh ? y0 + 1 : y0;
        for (uint32_t dx = 0; dx < dw; dx++) {
            uint32_t fx = dx * stepx, x0 = fx >> 16, wx = (fx >> 8) & 0xFF;
            uint32_t x1 = x0 + 1 < sw ? x0 + 1 : x0;
            uint32_t idx[4] = { y0 * sw + x0, y0 * sw + x1, y1 * sw + x0, y1 * sw + x1 };
            uint32_t wgt[4] = { (256 - wx) * (256 - wy), wx * (256 - wy),
                                (256 - wx) * wy,          wx * wy };
            uint32_t r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) {
                uint16_t c = original
                    ? (uint16_t)(col[idx[k] * 2] | (col[idx[k] * 2 + 1] << 8)) : tint;
                uint32_t a = alpha[idx[k]];
                /* Over black: each channel simply scales with alpha. */
                r += (((c >> 11) & 0x1F) * a / 255) * wgt[k];
                g += (((c >> 5) & 0x3F) * a / 255) * wgt[k];
                b += ((c & 0x1F) * a / 255) * wgt[k];
            }
            s_flat_px[dy * dw + dx] = (uint16_t)(((r >> 16) << 11) | ((g >> 16) << 5) | (b >> 16));
        }
    }
    s_flat.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_flat.header.cf = LV_COLOR_FORMAT_RGB565;
    s_flat.header.w = dw;
    s_flat.header.h = dh;
    s_flat.header.stride = dw * 2;
    s_flat.data_size = dn * 2;
    s_flat.data = (const uint8_t *)s_flat_px;
    s_flat_valid = true;
#if CONFIG_IDF_TARGET_ESP32P4
    /* What a move uncovers should match what the body sits on. flatten() composites
     * over black, so this is black today — read rather than assumed, so art that
     * reaches the corner still slides over its own background. */
    uint16_t c = s_flat_px[0];
    uint32_t r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
    s_body_bg = ((r5 << 3 | r5 >> 2) << 16) | ((g6 << 2 | g6 >> 4) << 8) | (b5 << 3 | b5 >> 2);
#endif
#if CONFIG_IDF_TARGET_ESP32P4
    /* Once, on the first body we flatten: what the hardware costs, before anything
     * is built on the assumption that it is fast. */
    static bool benched;
    if (!benched) { benched = true; face_ppa_benchmark(s_flat_px, dw, dh, dw, dh); }
#endif
    if (sw != dw)
        ESP_LOGI(TAG, "character baked at %lux%lu, resampled to the panel's %lux%lu",
                 (unsigned long)sw, (unsigned long)sh, (unsigned long)dw, (unsigned long)dh);
}

#if CONFIG_IDF_TARGET_ESP32P4
/* Called by LVGL before it renders a frame, with the buffer it is about to render
 * into. The body goes down first and LVGL draws the features over it — which is the
 * whole trick: LVGL never touches the 518,400 pixels underneath, so the expensive
 * layer moves in hardware while the cheap ones keep its transform stack. */
static void render_start_cb(lv_event_t *e)
{
    s_render_t0 = esp_timer_get_time();
    if (!s_flat_valid || !s_flat_px) return;
    lv_display_t *d = lv_event_get_current_target(e);
    lv_draw_buf_t *buf = lv_display_get_buf_active(d);
    if (!buf || !buf->data) return;

    esp_err_t err = face_ppa_body(s_flat_px, s_flat.header.w, s_flat.header.h,
                                  buf->data,
                                  lv_display_get_horizontal_resolution(d),
                                  lv_display_get_vertical_resolution(d),
                                  s_bx, s_by, s_body_bg);
    if (err != ESP_OK) {
        /* Never leave the screen half-painted over a transient: stop asking. */
        ESP_LOGW(TAG, "body blit (%+d,%+d): %s", s_bx, s_by, esp_err_to_name(err));
        s_bx = s_by = 0;
    }
}

/* What a whole rendered frame costs, body blit included — the timer starts at the
 * top of render_start_cb. Against face_ppa.c's own figure it splits the frame in
 * two, so the next thing to optimise is chosen from a number rather than a guess.
 * Measured here: 23-33 ms a frame, of which the body is 14-18 ms and LVGL's
 * features are the remaining 9-14 ms. */
static void render_ready_cb(lv_event_t *e)
{
    (void)e;
    static uint32_t n; static int64_t total, worst;
    int64_t us = esp_timer_get_time() - s_render_t0;
    total += us;
    if (us > worst) worst = us;
    if (++n == 100) {
        ESP_LOGI(TAG, "100 renders: mean %lld us, worst %lld us, body blit included",
                 total / 100, worst);
        n = 0; total = worst = 0;
    }
}
#endif

/* Re-flatten with whatever character and colour are current, and show the result. */
static void reflatten(void)
{
    const lv_image_dsc_t *src = s_char ? &s_char->bg : &face_bg;
    flatten(src, s_flat_rgb, s_flat_original);
    if (!s_bg) return;
    lv_image_set_src(s_bg, s_flat_valid ? &s_flat : src);
    /* The tint is baked in now, so LVGL must not apply it a second time. */
    lv_obj_set_style_image_recolor_opa(s_bg, LV_OPA_TRANSP, 0);
}

static void wear_character(const face_character_t *c)
{
    s_char = c;
    /* The catalogue's artwork parts and their offsets are in its own space. */
    s_cat_scale = (c && c->bg.header.w) ? (float)face_bg.header.w / c->bg.header.w : 1.0f;
    measure_features();              /* needs s_char and s_cat_scale, both just set */
    reflatten();
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
    s_flat_rgb = 0xFFFFFF;
    s_flat_original = true;
    reflatten();

#if CONFIG_IDF_TARGET_ESP32P4
    /* The body stops being an LVGL object here. It is hidden rather than deleted so
     * every other path in this file — reflatten(), paint_body(), wear_character() —
     * keeps working unchanged, and so the S3 build is untouched. */
    if (face_ppa_start() == ESP_OK) {
        lv_obj_add_flag(s_bg, LV_OBJ_FLAG_HIDDEN);
        /* And the screen must stop painting over it: LVGL fills the dirty area with
         * the screen's background before drawing children, which would erase the
         * body the PPA just laid down. Transparent, it leaves the buffer alone. */
        lv_obj_set_style_bg_opa(lv_obj_get_screen(s_root), LV_OPA_TRANSP, 0);
        lv_display_add_event_cb(lv_obj_get_display(s_root), render_start_cb,
                                LV_EVENT_RENDER_START, NULL);
        lv_display_add_event_cb(lv_obj_get_display(s_root), render_ready_cb,
                                LV_EVENT_RENDER_READY, NULL);
        ESP_LOGI(TAG, "body composited by the PPA; LVGL draws the features over it");
    } else {
        ESP_LOGW(TAG, "no PPA: the body stays an LVGL layer, and stays still");
    }
#endif

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
    s_flat_rgb = rgb;
    s_flat_original = original;
    reflatten();
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
    report_worn();
}

/* What each layer was last given, so an unchanged layer costs nothing.
 *
 * Every setter here marks its object dirty, and a dirty full-screen background is
 * 518,400 pixels of redraw — so calling them unconditionally made every frame a full
 * frame whether or not anything moved. gallery.c already learned this about its jaw
 * ("only touches LVGL on a change of a whole pixel"); the face had not.
 *
 * It also quietly fixes the breathing. Scale arrives as a float and leaves as 256ths,
 * so a 3 s breath cycle of a percent or two crosses an integer step a handful of
 * times a second, not sixty: the body still breathes, and the expensive transform
 * runs only on the frames where the result would actually differ. */
typedef struct { int32_t sx, sy, rot, x, y; lv_opa_t opa; bool set; } placed_t;
static placed_t s_placed[FACE_LAYER_COUNT];

/* Place one image under the face transform composed with its own.
 * `ox, oy` is the sprite's untransformed top-left; w, h its size. */
static void place(int slot, lv_obj_t *obj, const face_values_t *head, const face_values_t *own,
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
    placed_t p = {
        .sx  = (int32_t)(sx * own->scale * 256.0f),
        .sy  = (int32_t)(sy * own->scale * own->scale_y * 256.0f),
        .rot = (int32_t)((head->rotation + own->rotation) * 10.0f),
        .x   = (int32_t)lroundf(rx - w / 2.0f),
        .y   = (int32_t)lroundf(ry - h / 2.0f),
        .opa = (lv_opa_t)(head->opacity * own->opacity * 255.0f),
        .set = true,
    };
    placed_t *was = &s_placed[slot];
    if (was->set && p.sx == was->sx && p.sy == was->sy && p.rot == was->rot
        && p.x == was->x && p.y == was->y && p.opa == was->opa)
        return;                          /* nothing moved: leave the screen alone */
    *was = p;

    lv_image_set_pivot(obj, w / 2, h / 2);
    lv_image_set_scale_x(obj, p.sx);
    lv_image_set_scale_y(obj, p.sy);
    lv_image_set_rotation(obj, p.rot);
    lv_obj_set_pos(obj, p.x, p.y);
    lv_obj_set_style_opa(obj, p.opa, 0);
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
        s_preview_i = -1;          /* something real wants the face; stop showing off */
        if (want != s_state) face_view_set_state(want);
    }
    if (s_preview_req) {
        s_preview_req = false;
        s_preview_back = s_state;
        s_preview_i = 0;
        s_preview_t0 = elapsed_ms;
        face_view_set_state(0);
    } else if (s_preview_i >= 0 && elapsed_ms - s_preview_t0 >= PREVIEW_MS) {
        s_preview_t0 = elapsed_ms;
        if (++s_preview_i >= face_state_count) {
            s_preview_i = -1;
            face_view_set_state(s_preview_back);
        } else {
            face_view_set_state(s_preview_i);
        }
    }
    if (s_expr_pending) {
        s_expr_pending = false;
        if (s_expr_clearing) {
            s_expr_on = false;
        } else {
            for (int i = 0; i < 5; i++) s_expr[i] = s_want_expr[i];
            s_expr_on = true;
        }
        for (int i = 0; i < 5; i++) show_sprite(DESIGN_TO_LAYER[i], part_of(s_state, i));
        report_worn();
    }
    if (s_parts_dirty) {
        s_parts_dirty = false;
        for (int i = 0; i < 5; i++) show_sprite(DESIGN_TO_LAYER[i], part_of(s_state, i));
        report_worn();
    }

    face_values_t v[FACE_LAYER_COUNT];
    face_motion_sample(s_state, elapsed_ms, v);

#if CONFIG_BUDDY_FACE_MOTION_DEMO
    {
        /* Deliberately after the sample and on top of it: the point is to watch one
         * preset at a time on the head, not to blend it with the state's own. */
        const char *preset = NULL;
        if (face_demo_motion(elapsed_ms, &v[0], &preset))
            ESP_LOGI(TAG, "motion demo: %s", preset);
    }
#endif

    /* The body does not take the head's transform, and on this panel that is a
     * deliberate trade rather than an omission.
     *
     * Scaling a full-screen alpha image means inverse-mapping and interpolating
     * 518,400 pixels on one core with no SIMD. Measured on the board, at 720x720, a
     * frame that did it cost 330 ms against a 16 ms budget; the same frame without
     * it costs 12-141 ms, and the quiet frames either side fall from 35 ms to under
     * 2 ms. At the S3's 240x240 the same code moved a ninth of the pixels and nobody
     * noticed.
     *
     * The features still breathe — brows, eyes and mouth are small enough that
     * transforming them is free — so the face is alive; it is the body that holds
     * still.
     *
     * Measured since, on this board (see face_ppa.c): LVGL needs 48-62 ms just to
     * blit this layer opaque and unscaled, and 250-460 ms to scale it, while the
     * PPA does the same move in 14 ms. So on the P4 the body is not an LVGL layer
     * at all any more — render_start_cb puts it on the panel in hardware and this
     * call does nothing. The S3 keeps the still body, which is right for it. */
#if CONFIG_IDF_TARGET_ESP32P4
    {
        /* The head's own x/y, in the same face units place() uses, turned into
         * whole panel pixels — the PPA translates exactly, so there is no rounding
         * to hide and a one-pixel move is a one-pixel move. */
        const float UNIT = face_bg.header.w / 100.0f;
        int bx = (int)lroundf(v[0].x * UNIT), by = (int)lroundf(v[0].y * UNIT);
        if (bx != s_bx || by != s_by) {
            s_bx = bx; s_by = by;
            /* A head that moves while every feature happens to be still would
             * otherwise invalidate nothing, and the frame would never be drawn. */
            lv_obj_invalidate(lv_obj_get_screen(s_root));
        }
    }
#else
    static const face_values_t REST = { 0, 0, 1, 1, 0, 1, -1 };
    place(0, s_bg, &REST, &REST, 0, 0, face_bg.header.w, face_bg.header.h);
#endif

    for (int i = 1; i < FACE_LAYER_COUNT; i++) {
        if (i == MOUTH_LAYER && s_live_mouth >= 0) {
            /* Live speech wins over whatever the mouth track was doing. */
            show_sprite(i, s_live_mouth);
        } else if (i == MOUTH_LAYER && v[i].sprite < 0) {
            /* Released, and this state has no mouth track to fall back to: put the
             * designed mouth back rather than leaving the last spoken shape on. */
            show_sprite(i, part_of(s_state, 4));
        } else if (v[i].sprite >= 0) {
            /* A baked sprite track names absolute sprites, and the face it was
             * generated from is the factory's — so its "home" key is the factory
             * part for this state. Drawing that undid the child's design every time
             * a blink or a talk cycle came back round: `speaking` runs three sprite
             * tracks (both eyes blinking, the 480 ms mouth cycle), so a designed
             * speaking face was overwritten within a frame of the Buddy opening its
             * mouth, and the child never saw it.
             *
             * So home means "whatever the design says". Every other value is the
             * track doing something the design has no opinion about — a closed eye,
             * an open mouth — and is drawn as baked. A blink now closes the child's
             * eyes and opens them again as theirs. */
            int slot = i - 1;                  /* DESIGN_TO_LAYER is {1,2,3,4,5} */
            int16_t home = face_design[s_state][slot].sprite;
            show_sprite(i, v[i].sprite == home ? part_of(s_state, slot) : v[i].sprite);
        }
        if (s_shown[i] < 0 || lv_obj_has_flag(s_layer[i], LV_OBJ_FLAG_HIDDEN)) continue;
        if (!s_img[i]) continue;
        /* A catalogue part is drawn for a smaller face than this panel, so it
         * carries the same factor its offsets did. Firmware sprites are already in
         * the panel's space and get nothing. */
        face_values_t own = v[i];
        if (s_shown[i] == FACE_PART_ARTWORK) own.scale *= s_cat_scale;
        place(i, s_layer[i], &v[0], &own, s_ix[i], s_iy[i], s_img[i]->header.w, s_img[i]->header.h);
    }
}
