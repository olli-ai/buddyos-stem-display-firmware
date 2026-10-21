#pragma once
#include "lvgl.h"

/**
 * The setup screen: what a kit that has never been set up shows while it is being
 * set up.
 *
 * This kit has a screen, and the other STEM kits do not. On those, setup is done
 * blind — the parent reads a serial off a sticker, finds it in a list of Bluetooth
 * names, and then watches a web page that cannot see the kit. Everything the kit
 * knows about how it is going stays on a UART nobody has attached. Here it goes on
 * the glass: the serial to pick, and which step the kit thinks it is on.
 *
 * The serial matters most. It is the advertised BLE name, the manufacturer data the
 * website filters on, the MQTT username and the row the gateway creates — one string
 * that must match between a label, a browser and a broker. Shown large, because it is
 * read aloud and typed by someone holding a phone in the other hand.
 *
 * Written like status.c, and for the same reason: setup_ui_set() and
 * setup_ui_detail() only store, and an LVGL timer draws, so nothing calls LVGL from
 * the net, BLE or MQTT tasks.
 */
typedef enum {
    SETUP_UI_HIDDEN = 0,   /* not in setup: the gallery has the screen */
    SETUP_UI_WAITING,      /* advertising; the serial is the name to pick */
    SETUP_UI_LINKED,       /* a phone or a browser is connected over BLE */
    SETUP_UI_SCANNING,     /* listing the networks the kit can see */
    SETUP_UI_JOINING,      /* trying the credentials it was given */
    SETUP_UI_FAILED,       /* they did not work; returns to LINKED on its own */
    SETUP_UI_REGISTERING,  /* on Wi-Fi, telling the gateway who it is */
    SETUP_UI_DONE,         /* the gateway answered; hides itself */
} setup_ui_state_t;

/** Build the screen on `scr`, hidden, above whatever is already there. Last, so it
 *  covers the gallery and the status pill both. LVGL task only. */
void setup_ui_create(lv_obj_t *scr);

/** Any task.
 *
 *  While hidden, only SETUP_UI_WAITING is acted on: the screen is opened by setup
 *  starting and closed by it finishing, and a kit that was provisioned some other
 *  way must not flash a setup step at a child who never asked for one. */
void setup_ui_set(setup_ui_state_t s);

/** The one changing thing in the step line — the network being joined, or why it was
 *  refused. Set it before the state it belongs to. Any task. */
void setup_ui_detail(const char *text);
