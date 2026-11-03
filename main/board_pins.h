#pragma once
#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32P4
/*
 * Every GPIO this firmware drives, in one place.
 *
 * The ESP32-S3 kit: an ESP32-S3-WROOM-1 with a GC9A01 1.28" 240x240 round TFT on SPI,
 * and no touch layer. Octal PSRAM (35-37), native USB (19/20) and flash (26-32) are
 * untouched, and so are the strapping pins 3, 45 and 46. GPIO 0 is BOOT and nothing
 * else, so it can be read at any time.
 *
 * Two boards carry that description and share not one GPIO, so which one is a build
 * choice: BUDDY_S3_BOARD. Flashing the wrong one is not a subtle failure, but it is a
 * silent one — a dark screen and a silent speaker, with every register correct.
 */

#define BOARD_LCD_W     240
#define BOARD_LCD_H     240
#define BOARD_ROUND       1
#define BOARD_HAS_TOUCH   0    /* no touch layer: the wake word is the way back */

#if CONFIG_BUDDY_S3_BUDDYDISPLAY_REVA
/*
 * BuddyDisplay rev A (KiCad project BuddyDisplay, ESP32-S3-WROOM-1-N16R8), read from
 * the schematic netlist and cross-checked against the PCB pads. The schematic's text
 * notes still describe an older allocation (LCD on 9-14, amp on 5/6/7, BTN2 on 21, a
 * WS2812 on 48); the wiring itself is what is below.
 */
#define PIN_BOOT          0    /* BTN1. Also the sleep wake button and the boot reset */

/* How far the face is turned on the glass, degrees, positive clockwise.
 *
 * The panel is soldered down and the enclosure holds it at an angle, so the face has
 * to be turned the same amount to come out level. It belongs here for the reason the
 * mirror in gc9a01.c does: which way a fixed panel faces is a fact about the board.
 *
 * It is NOT a display rotation, and could not be — the GC9A01's MADCTL and LVGL's
 * lv_display_set_rotation both only do quarter turns. face_view.c turns the face
 * instead: the body's angle is resolved into its pixels once per character, and the
 * features carry it in the transform they already have. That works only because this
 * glass is round (BOARD_ROUND), so a turn uncovers no corner.
 *
 * What it does NOT turn is the text drawn over the face — the clock, the status pill,
 * the volume panel. LVGL draws any transformed object through an intermediate layer
 * except lv_image, and a layer is the allocation this firmware cannot make.
 *
 * The sign is the easy half to get wrong, and it is wrong in a way that looks right
 * until you hold the kit up: this is the turn applied to the *face*, so it has to
 * oppose the enclosure's, not match it. +30 on a board whose glass is already 30
 * clockwise does not level anything — it lands at 60 and reads as a much worse tilt
 * than the one being corrected. Seen on rev A on 2026-11-03, which is why it says -30
 * and not 30. If a face ever comes out leaning harder than the bare panel did, this
 * line is the first thing to look at. */
#define BOARD_FACE_ROT_DEG  (-30)

/* ---- GC9A01 1.28" round TFT, SPI (FPC DS501/DS502) ------------------------ */
/* R501 and R502 are drawn as 22 R and fitted as 100 R on rev A (measured 2026-10-28).
 * That cost a bring-up week before the glass turned out to be the fault, and the
 * replacement panel runs this bus at the full 40 MHz with the 100 R parts still on it.
 * So it is a note, not a limit — but check them before believing a timing figure. */
#define PIN_LCD_SCLK     12    /* SCL on the panel, through R501 */
#define PIN_LCD_MOSI     11    /* SDA on the panel, through R502 */
#define PIN_LCD_DC       16
#define PIN_LCD_CS       17    /* R503 pull-up: deselected until we drive it */
#define PIN_LCD_RST      18    /* R504 pull-up */
#define PIN_LCD_BL       13    /* Q501 gate: high lights the backlight, R507 holds it
                                * off through reset so the panel's power-on garbage is
                                * never seen */

/* ---- MAX98357A amplifier (U601) and INMP441 microphone (J602), two I2S buses */
/* Separate clocks on this board, so two controllers — the opposite of the bench
 * board's one. audio.c picks its topology off PIN_SPK_BCLK being defined. */
#define PIN_SPK_SD       14    /* SD_MODE: high enables (left slot), low mutes */
#define PIN_SPK_DIN      21
#define PIN_SPK_BCLK     47
#define PIN_SPK_LRC      48
#define PIN_MIC_SD       40    /* the mic's data OUT, an input to us */
#define PIN_MIC_SCK      38
#define PIN_MIC_WS       39
/* No PIN_SPK_GAIN: GAIN_SLOT is left open, so the gain is a fixed 9 dB. No PIN_MIC_LR:
 * the mic's L/R is tied to ground in the cable (J602 pin 6), so it always talks in the
 * left half of the frame.
 *
 * And no speaker connector: rev A has none. OUTP/OUTN leave U601 through FB602/FB603
 * and end on two test points, TP504 (SPK_P) and TP505 (SPK_N) — the speaker solders
 * straight to those pads. A board with the amplifier fitted and nothing on TP504/505
 * is silent with every register and every clock correct, which is an hour nobody
 * should spend twice. */

/* ---- Other rev A parts, not driven by this firmware yet ------------------- */
#define PIN_BTN2          1    /* active low. Silkscreen says IO21: it is IO1 */
#define PIN_BAT_ADC       8    /* ADC1_CH7, VBAT through a 100k/100k divider */
#define PIN_RGB_DATA      2    /* WS2812 footprint, not fitted on rev A */

#else /* CONFIG_BUDDY_S3_BENCH */
/*
 * The hand-wired GC9A01 bench board of 2026-09-28, where this panel first ran
 * (ESP32-S3-WROOM-1-N8R2).
 *
 * The audio is NOT that board's as first wired: it drove the amplifier and the
 * microphone from two I2S controllers on separate clocks (amp 9-13, mic 4-7). This is
 * one controller in full duplex, the amp and the mic sharing BCLK and WS — what
 * audio.c drives, and what voice was measured on with the 480x480 ST7701S screen this
 * kit had before (removed 2026-10-29; it is in git history). Wire a board to these
 * pins, or the speaker and the microphone will not both work.
 */
#define PIN_BOOT          0

/* A bare module on the bench with no enclosure to tilt it: the face sits level. */
#define BOARD_FACE_ROT_DEG    0

/* ---- GC9A01 1.28" round TFT, SPI ------------------------------------------ */
#define PIN_LCD_SCLK     15    /* SCL on the module */
#define PIN_LCD_MOSI     16    /* SDA on the module */
#define PIN_LCD_DC       17
#define PIN_LCD_CS       18
#define PIN_LCD_RST       8
/* No PIN_LCD_BL: the backlight is the module's own, BLK on 3.3 V, so it is lit
 * whenever the board has power and "display off" is the controller's DISPOFF. */

/* ---- MAX98357A amplifier and INMP441 microphone, one I2S bus -------------- */
/* Both parts share BCLK and WS, so they run on ONE I2S controller in full duplex: two
 * controllers would each drive their own clock onto the same wire. */
#define PIN_I2S_BCLK      5    /* amp BCLK, mic SCK */
#define PIN_I2S_WS        6    /* amp LRC,  mic WS  */
#define PIN_SPK_DIN       7    /* our data out, the amplifier's in */
#define PIN_MIC_SD        4    /* the mic's data OUT, an input to us */
/* Strapped on the board, not driven: amp SD through 100 k to VIN (on, left channel),
 * amp GAIN floating (9 dB), mic L/R to GND (left). */

#endif /* CONFIG_BUDDY_S3_BUDDYDISPLAY_REVA */

#else /* CONFIG_IDF_TARGET_ESP32P4 */
/*
 * Waveshare ESP32-P4-WIFI6-Touch-LCD-4C: an ESP32-P4 (rev 3) with 32 MB PSRAM and 16 MB
 * flash, a 4" 720x720 square JD9365 panel on two-lane MIPI-DSI with a GT911 touch layer,
 * an ES8311 codec driving the speaker amplifier and an ES7210 ADC behind two microphones,
 * and an ESP32-C6 beside it for Wi-Fi 6 and Bluetooth (esp_hosted over SDIO).
 *
 * Every pin is fixed by the board; these are the ones Waveshare's BSP uses. The P4 has
 * no radio of its own, so nothing here is Wi-Fi or BLE: that is the C6, reached through
 * esp_wifi_remote and esp_hosted, whose SDIO pins are esp_hosted's Kconfig defaults.
 */
#define BOARD_LCD_W     720
#define BOARD_LCD_H     720
/* The frame buffer is square and the glass is not: this board's 720x720 sits behind a
 * round bezel, so the corners are never seen. It said 0 until 2026-10-26, which cost a
 * volume bar drawn under the frame before anyone noticed. Anything placed near an edge
 * has to clear a circle of diameter min(w, h), not a rectangle.
 *
 * It is not a stand-in for "the small panel". Two things used it that way, because the
 * S3's 480 was the only round board at the time — the setup screen's font sizes and its
 * QR — and both now key off BOARD_LCD_W instead. Round says where the edges are; width
 * says how big to draw. */
#define BOARD_ROUND       1
#define BOARD_HAS_TOUCH   1
/* Mounted square in its own bezel, and it must stay 0 on this board: the P4's body is
 * blitted by the PPA, which turns in quarter circles only, so a turned face here would
 * tilt the features off a level head. face_view.c refuses to build otherwise. */
#define BOARD_FACE_ROT_DEG    0
#define PIN_BOOT         35    /* BOOT. Not an LP GPIO: it cannot wake the chip, see sleep.c */

/* ---- JD9365 720x720 MIPI-DSI panel ---------------------------------------- */
#define PIN_LCD_RST      27
#define PIN_LCD_BL       26    /* LEDC PWM, active low */
#define BOARD_DSI_LDO_CHAN   3 /* the DSI PHY's supply, from the P4's own LDO */
#define BOARD_DSI_LDO_MV  2500

/* ---- I2C: GT911 touch, ES8311, ES7210 share one bus ------------------------ */
#define PIN_I2C_SDA       7
#define PIN_I2C_SCL       8

/* ---- I2S: ES8311 out, ES7210 in, one controller --------------------------- */
#define PIN_I2S_MCLK     13
#define PIN_I2S_BCLK     12
#define PIN_I2S_WS       10
#define PIN_I2S_DOUT      9    /* to the ES8311 */
#define PIN_I2S_DIN      11    /* from the ES7210 */
#define PIN_PA_EN        53    /* the speaker amplifier's enable */

/* ---- ESP32-C6 (esp_hosted slave) ------------------------------------------ */
#define PIN_C6_RESET     54    /* held low in deep sleep, so the radio sleeps too */

#endif
