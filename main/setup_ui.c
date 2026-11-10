/* The setup screen. See setup_ui.h. */
#include "setup_ui.h"
#include "board_pins.h"
#include "prov.h"

#include "esp_log.h"
#include <string.h>

static const char *TAG = "setup";

/* 48 on the P4's 720x720, 28 on the S3's 480 round panel, and whatever is actually
 * compiled in if someone trims the font list — a setup screen that fails to build is
 * worse than one in a smaller face.
 *
 * On the WIDTH, not on BOARD_ROUND. This read `!BOARD_ROUND` while the S3's 480 was the
 * only round board, so the two were the same test by accident; the P4's glass is round
 * too, and the day its flag was corrected this would have dropped a 720 px screen to the
 * small panel's fonts. */
#define BIG_PANEL (BOARD_LCD_W >= 600)
/* And a third size for the GC9A01's 240: the serial `ESP32-cc8da2ece434` is ~290 px at
 * 28 pt, on a circle whose usable column is ~190 px. 14 pt is ~150. */
#define SMALL_PANEL (BOARD_LCD_W < 400)
#if SMALL_PANEL
#define SERIAL_FONT LV_FONT_DEFAULT
#elif LV_FONT_MONTSERRAT_48 && BIG_PANEL
#define SERIAL_FONT (&lv_font_montserrat_48)
#elif LV_FONT_MONTSERRAT_28
#define SERIAL_FONT (&lv_font_montserrat_28)
#else
#define SERIAL_FONT LV_FONT_DEFAULT
#endif
#if SMALL_PANEL
#define BODY_FONT   LV_FONT_DEFAULT
#elif LV_FONT_MONTSERRAT_28 && BIG_PANEL
#define BODY_FONT   (&lv_font_montserrat_28)
#elif LV_FONT_MONTSERRAT_20
#define BODY_FONT   (&lv_font_montserrat_20)
#else
#define BODY_FONT   LV_FONT_DEFAULT
#endif
#if SMALL_PANEL
#define HINT_FONT   LV_FONT_DEFAULT
#elif LV_FONT_MONTSERRAT_20
#define HINT_FONT   (&lv_font_montserrat_20)
#else
#define HINT_FONT   LV_FONT_DEFAULT
#endif

/* The two fixed strings, named so both the laid-out and the turned build say the same
 * words and a change here cannot reach only one of them. */
#define TITLE_TEXT  "Set this kit up"
#define HINT_TEXT   "BuddyOS STEM website > Set up with Bluetooth"

/* The column and the QR, as fractions of the panel. These were written inline in
 * setup_ui_create() and are hoisted because the turned build has to lay the screen out
 * arithmetically — it cannot ask flex how tall anything came out. */
#define COL_PCT     (SMALL_PANEL ? 80 : BOARD_ROUND ? 66 : 84)
#define COL_W       (BOARD_LCD_W * COL_PCT / 100)
#define PAD_ROW     (BOARD_ROUND ? 10 : 18)
#define QR_SIZE     (BOARD_LCD_W * (SMALL_PANEL ? 40 : 31) / 100)
#define QR_QUIET    (BOARD_LCD_W / 80)

/* A failure is read, not acted on: the app is already showing its own screen and the
 * kit is back to advertising, so the words go away on their own. */
#define FAILED_MS   6000
/* Long enough to be seen from across a room, short enough that the first thing the
 * kit does after setup is still show photos. */
#define DONE_MS     3000

static volatile setup_ui_state_t s_want = SETUP_UI_HIDDEN;
/* Written by the net, BLE or MQTT task; read by the LVGL task. Neither is locked:
 * the worst a torn read can do is draw one wrong label for 100 ms, and the states
 * that carry a detail are the ones a human is reading slowly anyway. */
static char s_detail[80];
static volatile uint32_t s_detail_gen;

static setup_ui_state_t s_shown = (setup_ui_state_t)-1;
static uint32_t s_shown_gen = (uint32_t)-1;
static uint32_t s_entered;             /* lv_tick when s_shown was drawn */

static lv_obj_t *s_box;
#if LV_USE_QRCODE
static lv_obj_t *s_qr;
#endif

/*
 * On a kit whose case turns the glass, this screen turns with it. It is the one a parent
 * reads word by word off a phone, so a tilted serial is worse here than anywhere else
 * on the kit.
 *
 * Three blocks of text, each rasterised into its own canvas and turned as an lv_image
 * (turned.c has the reason that is the only shape LVGL will transform for free), and
 * the flex column is gone: a turned block is placed, not laid out, so the heights are
 * added up here instead. relayout() is that sum, and it runs on a state change, which
 * is also the only time any of this is rasterised.
 *
 * The QR gets a canvas of its own rather than turning the lv_qrcode object, for two
 * reasons that both have to hold. Its buffer is LV_COLOR_FORMAT_I1 and the software
 * renderer's transform does not take indexed pixels — it would draw nothing. And its
 * white quiet zone is an object border, which is drawn square around the widget's box
 * while the image inside it turned: a rotated QR would push its own corners out past
 * the white and onto the black. Drawing white into a canvas, then the QR into the
 * middle of it, then turning the canvas, keeps the margin where a scanner needs it.
 */
#include "turned.h"
#include "face_rot.h"
#include "esp_heap_caps.h"

static turned_t s_ttop, s_tstep, s_thint;
static uint8_t *s_qrpx;                /* the QR and its quiet zone: PSRAM, ARGB8888 */
static lv_obj_t *s_qrc;
#define QR_BOX  (QR_SIZE + 2 * QR_QUIET)
/* ...and the level screen's four labels. Both sets exist; face_rot_deg() decides
 * which is built in setup_ui_create(). */
static lv_obj_t *s_title, *s_serial, *s_step, *s_hint;

void setup_ui_set(setup_ui_state_t s)
{
    if (s_want == SETUP_UI_HIDDEN && s != SETUP_UI_WAITING) return;
    s_want = s;
}

void setup_ui_detail(const char *text)
{
    strlcpy(s_detail, text ? text : "", sizeof(s_detail));
    s_detail_gen++;
}

static const struct { const char *step; uint32_t rgb; bool fmt; } k_look[] = {
    [SETUP_UI_HIDDEN]      = { "",                             0x000000, false },
    [SETUP_UI_WAITING]     = { "Pick this name in the app",    0xffb300, false },
    [SETUP_UI_LINKED]      = { "Connected. Choose your Wi-Fi", 0x1e88e5, false },
    [SETUP_UI_SCANNING]    = { "Looking for networks...",      0x1e88e5, false },
    [SETUP_UI_JOINING]     = { "Joining %s...",                0xffb300, true  },
    [SETUP_UI_FAILED]      = { "%s",                           0xe53935, true  },
    [SETUP_UI_REGISTERING] = { "On Wi-Fi. Registering...",     0x1e88e5, false },
    [SETUP_UI_DONE]        = { "Ready",                        0x43a047, false },
};

#if LV_USE_QRCODE
/* Draw the QR into its canvas once, the first time the screen is shown. Not in
 * setup_ui_create(): lv_canvas_finish_layer() dispatches the draw task and waits for
 * it to come back, and an LVGL timer is the only context this firmware does that from.
 * The serial never changes while the kit is on, so once is all it is. */
static void qr_once(void)
{
    static bool done;
    if (done || !s_qrpx || !s_qr) return;
    done = true;

    lv_canvas_set_buffer(s_qrc, s_qrpx, QR_BOX, QR_BOX, LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(s_qrc, lv_color_white(), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(s_qrc, &layer);
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = lv_canvas_get_draw_buf(s_qr);     /* the I1 bitmap the encoder filled in */
    lv_area_t a = { QR_QUIET, QR_QUIET, QR_QUIET + QR_SIZE - 1, QR_QUIET + QR_SIZE - 1 };
    lv_draw_image(&layer, &d, &a);
    lv_canvas_finish_layer(s_qrc, &layer);

    lv_image_set_pivot(s_qrc, QR_BOX / 2, QR_BOX / 2);
    lv_image_set_rotation(s_qrc, face_rot_deg() * 10);
    lv_obj_remove_flag(s_qrc, LV_OBJ_FLAG_HIDDEN);
}
#endif

/* The column flex would have laid out, added up by hand, because nothing here is a
 * child of anything: each block is placed at its own point on the face's axes. */
static void relayout(setup_ui_state_t s)
{
#if LV_USE_QRCODE
    qr_once();
#endif
    char step[96];
    if (k_look[s].fmt) lv_snprintf(step, sizeof(step), k_look[s].step, s_detail);
    else lv_snprintf(step, sizeof(step), "%s", k_look[s].step);
    const bool hint_on = (s == SETUP_UI_WAITING);

    const turned_line_t top[2] = {
        { TITLE_TEXT,    HINT_FONT,   0x9e9e9e },
        { prov_serial(), SERIAL_FONT, 0xffffff },
    };
    const turned_line_t st[1]   = { { step,      BODY_FONT, k_look[s].rgb } };
    const turned_line_t hint[1] = { { HINT_TEXT, HINT_FONT, 0x616161 } };

    int32_t w, h_top, h_step, h_hint;
    turned_measure(top,  2, COL_W, PAD_ROW, &w, &h_top);
    turned_measure(st,   1, COL_W, PAD_ROW, &w, &h_step);
    turned_measure(hint, 1, COL_W, PAD_ROW, &w, &h_hint);
    /* What turned_set will actually draw, not what the words asked for, so the column
     * still adds up on the one state whose text comes off the air — a network named
     * something long is a clipped block, and the rest must not slide for it. */
    h_top  = LV_MIN(h_top,  s_ttop.maxh);
    h_step = LV_MIN(h_step, s_tstep.maxh);
    h_hint = hint_on ? LV_MIN(h_hint, s_thint.maxh) : 0;

    int32_t qrh = 0;
#if LV_USE_QRCODE
    if (s_qrpx) qrh = QR_BOX;
#endif

    const int32_t n = 2 + (qrh ? 1 : 0) + (h_hint ? 1 : 0);
    int32_t y = -(h_top + qrh + h_step + h_hint + PAD_ROW * (n - 1)) / 2;

    turned_set(&s_ttop, top, 2, 0, y + h_top / 2);
    y += h_top + PAD_ROW;
#if LV_USE_QRCODE
    if (qrh) {
        int32_t qx, qy;
        turned_point(0, y + qrh / 2, &qx, &qy);
        lv_obj_align(s_qrc, LV_ALIGN_CENTER, qx, qy);
        y += qrh + PAD_ROW;
    }
#endif
    turned_set(&s_tstep, st, 1, 0, y + h_step / 2);
    y += h_step + PAD_ROW;
    if (hint_on) turned_set(&s_thint, hint, 1, 0, y + h_hint / 2);
    /* Only while the kit is waiting to be found: once a phone is talking to it, the
     * phone is giving the instructions and two sets of them disagree sooner or later. */
    turned_show(&s_thint, hint_on);
}

static void on_tick(lv_timer_t *t)
{
    (void)t;
    setup_ui_state_t s = s_want;
    uint32_t now = lv_tick_get();

    /* The two states that end themselves. Done here rather than in net.c so the
     * network code never has to know how long a person needs to read something. */
    if (s == SETUP_UI_FAILED && s == s_shown && lv_tick_elaps(s_entered) > FAILED_MS)
        s_want = s = SETUP_UI_LINKED;
    else if (s == SETUP_UI_DONE && s == s_shown && lv_tick_elaps(s_entered) > DONE_MS)
        s_want = s = SETUP_UI_HIDDEN;

    uint32_t gen = s_detail_gen;
    if (s == s_shown && gen == s_shown_gen) return;
    s_shown = s;
    s_shown_gen = gen;
    s_entered = now;

    if (s == SETUP_UI_HIDDEN) { lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_HIDDEN);

    if (face_rot_deg()) { relayout(s); } else {
    if (k_look[s].fmt) lv_label_set_text_fmt(s_step, k_look[s].step, s_detail);
    else lv_label_set_text(s_step, k_look[s].step);
    lv_obj_set_style_text_color(s_step, lv_color_hex(k_look[s].rgb), 0);

    /* Only while the kit is waiting to be found: once a phone is talking to it, the
     * phone is giving the instructions and two sets of them disagree sooner or later. */
    if (s == SETUP_UI_WAITING) lv_obj_remove_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t rgb)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    return l;
}

#if LV_USE_QRCODE
/* The serial, for the camera. Typing `ESP32-e8f60ae99b71` off a screen into a phone is
 * the step people get wrong, and a wrong character looks exactly like a kit that will
 * not pair.
 *
 * It carries the serial alone, not a URL. The firmware does not know the website's
 * address — that differs per environment and is not its business — and a bare serial
 * is what the scanner needs either way. The printed serial stays directly above it,
 * because a camera that will not focus is not a reason to be unable to set a kit up.
 *
 * A white quiet zone goes round it: a scanner needs the margin, and this panel's
 * background is dark enough that without one the finder patterns run into it. Where
 * the glass is level that margin is this object's border; where it is turned, qr_once()
 * paints it into the canvas instead, because a border is drawn square whatever the
 * image inside it is doing.
 *
 * QR_SIZE is just under a third of the panel, which is what the two hand-picked sizes
 * it replaces both were — 150 of the S3's 480, 230 of the P4's 720. Written as a
 * fraction so a round 720 gets a QR a phone can actually resolve rather than the small
 * panel's, which is what keying this off BOARD_ROUND would now hand it. On the 240 a
 * third is 74 px, three pixels a module — too fine for a phone at arm's length — so
 * the small panel gives it 40%. */
static void make_qr(lv_obj_t *parent)
{
    s_qr = lv_qrcode_create(parent);
    lv_qrcode_set_size(s_qr, QR_SIZE);
    lv_qrcode_set_dark_color(s_qr, lv_color_black());
    lv_qrcode_set_light_color(s_qr, lv_color_white());
    lv_result_t qr = lv_qrcode_update(s_qr, prov_serial(), strlen(prov_serial()));
    ESP_LOGI(TAG, "setup QR for '%s': %s", prov_serial(),
             qr == LV_RESULT_OK ? "ready" : "REFUSED by the encoder");
    if (!face_rot_deg()) {
    lv_obj_set_style_border_color(s_qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_qr, QR_QUIET, 0);
    } else {
    /* Never shown: on a turned screen this object exists only to hold the encoder's
     * bitmap, which qr_once() copies into the canvas that does get shown. It is kept
     * rather than deleted — its I1 buffer is about a kilobyte of LVGL's pool, and
     * destroying an object whose draw buffer the image cache may still be holding is
     * not worth saving that. */
    lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif /* LV_USE_QRCODE */

void setup_ui_create(lv_obj_t *scr)
{
    s_box = lv_obj_create(scr);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_box, LV_OPA_COVER, 0);
    /* Clickable, and deliberately: it is over the gallery, and a swipe meant for a
     * setup screen must not turn the photo underneath. */
    lv_obj_add_flag(s_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_SCROLLABLE);

    if (!face_rot_deg()) {
    lv_obj_set_flex_flow(s_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_box, PAD_ROW, 0);

    /* On a round panel the corners of the frame buffer are not on the glass, so the
     * text lives in the inscribed square with room to spare rather than in the width. */
    lv_obj_t *col = lv_obj_create(s_box);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_PCT(COL_PCT), LV_SIZE_CONTENT);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, PAD_ROW, 0);

    s_title = label(col, HINT_FONT, 0x9e9e9e);
    lv_label_set_text(s_title, TITLE_TEXT);

    /* The serial, as the app will show it: the advertised BLE name, character for
     * character. Not upper-cased to be easier to read aloud — it has to match a
     * string in a list on a phone, and the two must look identical. */
    s_serial = label(col, SERIAL_FONT, 0xffffff);
    lv_label_set_text(s_serial, prov_serial());

#if LV_USE_QRCODE
    make_qr(col);
#endif

    s_step = label(col, BODY_FONT, 0xffb300);
    lv_label_set_text(s_step, "");

    s_hint = label(col, HINT_FONT, 0x616161);
    lv_label_set_text(s_hint, HINT_TEXT);
    } else {
    /* The same column, as blocks that will be placed rather than laid out. Each gets
     * room for its text to wrap once more than it does today: the title and the hint
     * are fixed strings, but the serial comes from the chip and the step from the
     * network, and a block that cannot grow is a clipped word on the one screen whose
     * whole job is to be read correctly. */
    const int32_t h_hint   = lv_font_get_line_height(HINT_FONT);
    const int32_t h_serial = lv_font_get_line_height(SERIAL_FONT);
    const int32_t h_body   = lv_font_get_line_height(BODY_FONT);
    turned_create(&s_ttop,  s_box, COL_W, h_hint * 2 + PAD_ROW + h_serial * 2, PAD_ROW);
    turned_create(&s_tstep, s_box, COL_W, h_body * 3, PAD_ROW);
    turned_create(&s_thint, s_box, COL_W, h_hint * 3, PAD_ROW);

#if LV_USE_QRCODE
    make_qr(s_box);
    s_qrc = lv_canvas_create(s_box);
    lv_obj_remove_flag(s_qrc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_qrc, LV_OBJ_FLAG_HIDDEN);
    const size_t need =
        (size_t)lv_draw_buf_width_to_stride(QR_BOX, LV_COLOR_FORMAT_ARGB8888) * QR_BOX;
    s_qrpx = heap_caps_aligned_alloc(64, (need + 63) & ~(size_t)63,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_qrpx) {
        ESP_LOGI(TAG, "turned QR %dx%d, %u B of PSRAM", (int)QR_BOX, (int)QR_BOX,
                 (unsigned)need);
    } else {
        /* The serial is printed directly above it and relayout() closes the gap, so a
         * kit with no room for this still sets up — by typing, which is the path that
         * has to work anyway. */
        ESP_LOGW(TAG, "no PSRAM for the turned QR (%u B); the printed serial is the way in",
                 (unsigned)need);
    }
#endif
    }

    lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(on_tick, 100, NULL);
}
