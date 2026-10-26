#pragma once
/*
 * Every GPIO this firmware drives, in one place.
 *
 * Bench board: ESP32-S3-WROOM-1-N16R8, rewired 2026-09-28. Checked against what the
 * S3 reserves on this module — octal PSRAM (35-37), native USB (19/20), flash
 * (26-32) and the strapping pins (0, 3, 45, 46) — and none of them is used here.
 * GPIO 3 in particular is gone: it is sampled at reset, and an I2S word clock on
 * it was the first suspect when the previous board would not boot cleanly.
 *
 * The original N8R2 bench board was wired differently (panel SCK 14, MOSI 10,
 * DC 11, CS 12, RST 13; amp SD 5, DIN 4, BCLK 6, LRC 7; mic SD 17, SCK 16, WS 9,
 * LR 18). It needs these changed, or rewiring, to run this build.
 */

/* ---- GC9A01 1.28" round TFT, SPI ---------------------------------------- */
#define PIN_LCD_SCLK   15      /* SCL on the module */
#define PIN_LCD_MOSI   16      /* SDA on the module */
#define PIN_LCD_DC     17
#define PIN_LCD_CS     18
#define PIN_LCD_RST     8

/* ---- MAX98357A amplifier, I2S out --------------------------------------- */
#define PIN_SPK_SD     13      /* shutdown: high enables, low mutes */
#define PIN_SPK_GAIN   12      /* see audio_set_gain() for what each level means */
#define PIN_SPK_DIN    11
#define PIN_SPK_BCLK   10
#define PIN_SPK_LRC     9

/* ---- INMP441 microphone, I2S in ----------------------------------------- */
#define PIN_MIC_SD      4      /* the mic's data OUT, an input to us */
#define PIN_MIC_SCK     5
#define PIN_MIC_WS      6
#define PIN_MIC_LR      7      /* channel select: held low = left */
