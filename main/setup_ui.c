/* The setup screen. See setup_ui.h. */
#include "setup_ui.h"
#include "board_pins.h"
#include "prov.h"

#include <string.h>

/* 48 on the P4's 720x720, 28 on the S3's 480 round panel, and whatever is actually
 * compiled in if someone trims the font list — a setup screen that fails to build is
 * worse than one in a smaller face. */
#if LV_FONT_MONTSERRAT_48 && !BOARD_ROUND
#define SERIAL_FONT (&lv_font_montserrat_48)
#elif LV_FONT_MONTSERRAT_28
#define SERIAL_FONT (&lv_font_montserrat_28)
#else
#define SERIAL_FONT LV_FONT_DEFAULT
#endif
#if LV_FONT_MONTSERRAT_28 && !BOARD_ROUND
#define BODY_FONT   (&lv_font_montserrat_28)
#elif LV_FONT_MONTSERRAT_20
#define BODY_FONT   (&lv_font_montserrat_20)
#else
#define BODY_FONT   LV_FONT_DEFAULT
#endif
#if LV_FONT_MONTSERRAT_20
#define HINT_FONT   (&lv_font_montserrat_20)
#else
#define HINT_FONT   LV_FONT_DEFAULT
#endif

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

static lv_obj_t *s_box, *s_title, *s_serial, *s_step, *s_hint;

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

    if (k_look[s].fmt) lv_label_set_text_fmt(s_step, k_look[s].step, s_detail);
    else lv_label_set_text(s_step, k_look[s].step);
    lv_obj_set_style_text_color(s_step, lv_color_hex(k_look[s].rgb), 0);

    /* Only while the kit is waiting to be found: once a phone is talking to it, the
     * phone is giving the instructions and two sets of them disagree sooner or later. */
    if (s == SETUP_UI_WAITING) lv_obj_remove_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_set_flex_flow(s_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_box, BOARD_ROUND ? 10 : 18, 0);

    /* On a round panel the corners of the frame buffer are not on the glass, so the
     * text lives in the inscribed square with room to spare rather than in the width. */
    lv_obj_t *col = lv_obj_create(s_box);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_PCT(BOARD_ROUND ? 66 : 84), LV_SIZE_CONTENT);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, BOARD_ROUND ? 10 : 18, 0);

    s_title = label(col, HINT_FONT, 0x9e9e9e);
    lv_label_set_text(s_title, "Set this kit up");

    /* The serial, as the app will show it: the advertised BLE name, character for
     * character. Not upper-cased to be easier to read aloud — it has to match a
     * string in a list on a phone, and the two must look identical. */
    s_serial = label(col, SERIAL_FONT, 0xffffff);
    lv_label_set_text(s_serial, prov_serial());

    s_step = label(col, BODY_FONT, 0xffb300);
    lv_label_set_text(s_step, "");

    s_hint = label(col, HINT_FONT, 0x616161);
    lv_label_set_text(s_hint, "BuddyOS STEM website > Set up with Bluetooth");

    lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(on_tick, 100, NULL);
}
