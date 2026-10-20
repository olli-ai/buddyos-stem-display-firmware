#pragma once
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
 *     and the face loses half its red. The panel lets go of it in deep sleep, which is
 *     why it can still be the wake button — see sleep.c and net_boot_reset_check().
 *   - GPIO 3 and 46 are panel data. Both are sampled at reset; the panel's inputs are
 *     high-impedance until it is initialised, so the module's own straps win. Proven
 *     on the bench by the colour/gradient test in the panel bring-up project.
 *   - GPIO 21, which this file once named as the fallback wake pin, is now PCLK.
 *
 * Earlier boards: the GC9A01 SPI build (panel SCLK 15, MOSI 16, DC 17, CS 18, RST 8; amp
 * SD 13, GAIN 12, DIN 11, BCLK 10, LRC 9; mic SD 4, SCK 5, WS 6, LR 7) and before that
 * the N8R2 board. Neither runs this build.
 */

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

/* ---- MAX98357A amplifier and INMP441 microphone, one I2S bus -------------- */
/* Both parts share BCLK and WS, so they run on ONE I2S controller in full duplex: two
 * controllers would each drive their own clock onto the same wire. */
#define PIN_I2S_BCLK      5    /* amp BCLK, mic SCK */
#define PIN_I2S_WS        6    /* amp LRC,  mic WS  */
#define PIN_SPK_DIN       7    /* our data out, the amplifier's in */
#define PIN_MIC_SD        4    /* the mic's data OUT, an input to us */
/* Strapped on the board, not driven: amp SD through 100 k to VIN (on, left channel),
 * amp GAIN floating (9 dB), mic L/R to GND (left). */
