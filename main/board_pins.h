#pragma once
#include "sdkconfig.h"

#if CONFIG_BUDDY_S3_PANEL_GC9A01
/*
 * Every GPIO this firmware drives, in one place.
 *
 * The ESP32-S3 kit with a GC9A01 1.28" 240x240 round TFT on SPI, and no touch layer.
 * ESP32-S3-WROOM-1-N16R8: octal PSRAM (35-37), native USB (19/20) and flash (26-32) are
 * untouched, and so are the strapping pins 3, 45 and 46. GPIO 0 is BOOT and nothing
 * else, so unlike the ST7701S board it can be read at any time.
 *
 * The panel pins are the 2026-09-28 GC9A01 bench board's, where this panel first ran.
 * The audio is NOT that board's: it drove the amplifier and the microphone from two
 * I2S controllers on separate clocks (amp 9-13, mic 4-7). This follows the ST7701S
 * board instead — one controller in full duplex, the amp and the mic sharing BCLK and
 * WS — which is what audio.c drives and what voice has been measured on. Wire a board
 * to these pins, or the speaker and the microphone will not both work.
 */

#define BOARD_LCD_W     240
#define BOARD_LCD_H     240
#define BOARD_ROUND       1
#define BOARD_HAS_TOUCH   0    /* no touch layer: the wake word is the way back */
#define PIN_BOOT          0

/* ---- GC9A01 1.28" round TFT, SPI ------------------------------------------ */
#define PIN_LCD_SCLK     15    /* SCL on the module */
#define PIN_LCD_MOSI     16    /* SDA on the module */
#define PIN_LCD_DC       17
#define PIN_LCD_CS       18
#define PIN_LCD_RST       8
/* The backlight is the module's own: BLK is on 3.3 V, so it is lit whenever the board
 * has power, and "display off" is the controller's DISPOFF. */

/* ---- MAX98357A amplifier and INMP441 microphone, one I2S bus -------------- */
/* As on the ST7701S board: both parts share BCLK and WS, so they run on ONE I2S
 * controller in full duplex — two controllers would each drive their own clock onto
 * the same wire. */
#define PIN_I2S_BCLK      5    /* amp BCLK, mic SCK */
#define PIN_I2S_WS        6    /* amp LRC,  mic WS  */
#define PIN_SPK_DIN       7    /* our data out, the amplifier's in */
#define PIN_MIC_SD        4    /* the mic's data OUT, an input to us */
/* Strapped on the board, not driven: amp SD through 100 k to VIN (on, left channel),
 * amp GAIN floating (9 dB), mic L/R to GND (left). */

#elif !CONFIG_IDF_TARGET_ESP32P4
/*
 * Every GPIO this firmware drives, in one place.
 *
 * Bench board: ESP32-S3-WROOM-1-N16R8, rewired 2026-10-20 for the HD21001C40 — a 2.1"
 * 480x480 round ST7701S panel on a 40-pin FPC — with an HCY-CTP00021-A0 capacitive
 * touch layer on the same FPC. Octal PSRAM (35-37), native USB (19/20) and flash
 * (26-32) are still untouched.
 *
 * An RGB panel takes 20 pins and this one has them, so the strapping pins are no longer
 * avoided — they cannot be:
 *
 *   - GPIO 0 is DB17, the top bit of red. It is also BOOT. Nothing may read it while the
 *     panel runs: configuring it as an input takes the pin away from the LCD peripheral
 *     and the picture loses half its red. The panel lets go of it in deep sleep, which is
 *     why it can still be the wake button — see sleep.c and net_boot_reset_check().
 *   - GPIO 3 and 46 are panel data. Both are sampled at reset; the panel's inputs are
 *     high-impedance until it is initialised, so the module's own straps win. Proven
 *     on the bench by the colour/gradient test in the panel bring-up project.
 *   - GPIO 21, which this file once named as the fallback wake pin, is now PCLK.
 *
 * The GC9A01 kit is the other S3 board, above (CONFIG_BUDDY_S3_PANEL_GC9A01).
 */

#define BOARD_LCD_W     480
#define BOARD_LCD_H     480
#define BOARD_ROUND       1    /* the corners of the frame buffer are not on the glass */
#define BOARD_HAS_TOUCH   1
#define PIN_BOOT          0    /* = DB17 while the panel runs; see above */

/* ---- ST7701S 480x480 RGB panel (HD21001C40) ------------------------------ */
/* Configured once over 3-wire 9-bit SPI, bit-banged, then driven over a 16-bit RGB
 * bus. SCK and SDA are the same wires as B1/B2: CS stays high (10 k pull-up) once
 * the init sequence is sent, so the panel ignores them as SPI from then on. */
#define PIN_LCD_SPI_CS   42
#define PIN_LCD_SPI_SCK   1    /* = DB1 / B1 */
#define PIN_LCD_SPI_SDA   2    /* = DB2 / B2 */
#define PIN_LCD_RST      41    /* shared with TP_RST: one reset pulse resets both */

#define PIN_LCD_PCLK     21
#define PIN_LCD_DE       38
#define PIN_LCD_VSYNC    39
#define PIN_LCD_HSYNC    40

/* RGB565 onto an 18-bit bus: the LSB of red and of blue (DB12, DB0) are tied to GND.
 * The order is what esp_lcd calls data0..data15 — B1-B5, G0-G5, R1-R5. */
#define PIN_LCD_DATA { 1, 2, 3, 8, 9,            /* B1..B5 (DB1-DB5)   */ \
                       10, 11, 12, 13, 14, 15,   /* G0..G5 (DB6-DB11)  */ \
                       16, 17, 18, 46, 0 }       /* R1..R5 (DB13-DB17) */

/* The backlight is not ours: LEDA is on 5 V and each LEDK through its own resistor to
 * GND, so it is lit whenever the board has power. "Display off" is the panel's own
 * DISPOFF + SLPIN — black, and still faintly glowing. */

/* ---- HCY-CTP00021-A0 touch, I2C ------------------------------------------- */
#define PIN_TP_SDA       47    /* 4.7 k pull-up */
#define PIN_TP_SCL       48    /* 4.7 k pull-up */
/* TP_RST is PIN_LCD_RST. TP_INT is not wired: the touch is polled. */
#define PIN_I2C_SDA      PIN_TP_SDA   /* the board's I2C bus: see board_i2c.h */
#define PIN_I2C_SCL      PIN_TP_SCL

/* ---- MAX98357A amplifier and INMP441 microphone, one I2S bus -------------- */
/* Both parts share BCLK and WS, so they run on ONE I2S controller in full duplex: two
 * controllers would each drive their own clock onto the same wire. */
#define PIN_I2S_BCLK      5    /* amp BCLK, mic SCK */
#define PIN_I2S_WS        6    /* amp LRC,  mic WS  */
#define PIN_SPK_DIN       7    /* our data out, the amplifier's in */
#define PIN_MIC_SD        4    /* the mic's data OUT, an input to us */
/* Strapped on the board, not driven: amp SD through 100 k to VIN (on, left channel),
 * amp GAIN floating (9 dB), mic L/R to GND (left). */

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
