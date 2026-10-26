/* The ST7701S panel. See st7701.h. */
#include "st7701.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_idf_version.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_sig_map.h"

static const char *TAG = "st7701";

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint16_t delay_ms;
    uint8_t data[16];
} lcd_cmd_t;

/* The supplier sequence for 2.1" 480x480 round ST7701S glass (the Waveshare
 * ESP32-S3-Touch-LCD-2.1 uses the same), as proven on this board. Two lines are ours:
 * COLMOD 0x66 because the bus is 18 bits with the two LSBs grounded, and inversion off. */
static const lcd_cmd_t init_cmds[] = {
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x10}},
    {0xC0, 2, 0, {0x3B, 0x00}},
    {0xC1, 2, 0, {0x0B, 0x02}},
    {0xC2, 2, 0, {0x07, 0x02}},
    {0xCC, 1, 0, {0x10}},
    {0xCD, 1, 0, {0x08}},
    {0xB0, 16, 0, {0x00, 0x11, 0x16, 0x0E, 0x11, 0x06, 0x05, 0x09, 0x08, 0x21, 0x06, 0x13, 0x10, 0x29, 0x31, 0x18}},
    {0xB1, 16, 0, {0x00, 0x11, 0x16, 0x0E, 0x11, 0x07, 0x05, 0x09, 0x09, 0x21, 0x05, 0x13, 0x11, 0x2A, 0x31, 0x18}},

    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x11}},
    {0xB0, 1, 0, {0x6D}},
    {0xB1, 1, 0, {0x37}},
    {0xB2, 1, 0, {0x81}},
    {0xB3, 1, 0, {0x80}},
    {0xB5, 1, 0, {0x43}},
    {0xB7, 1, 0, {0x85}},
    {0xB8, 1, 0, {0x20}},
    {0xC1, 1, 0, {0x78}},
    {0xC2, 1, 0, {0x78}},
    {0xD0, 1, 0, {0x88}},
    {0xE0, 3, 0, {0x00, 0x00, 0x02}},
    {0xE1, 11, 0, {0x03, 0xA0, 0x00, 0x00, 0x04, 0xA0, 0x00, 0x00, 0x00, 0x20, 0x20}},
    {0xE2, 13, 0, {0}},
    {0xE3, 4, 0, {0x00, 0x00, 0x11, 0x00}},
    {0xE4, 2, 0, {0x22, 0x00}},
    {0xE5, 16, 0, {0x05, 0xEC, 0xA0, 0xA0, 0x07, 0xEE, 0xA0, 0xA0}},
    {0xE6, 4, 0, {0x00, 0x00, 0x11, 0x00}},
    {0xE7, 2, 0, {0x22, 0x00}},
    {0xE8, 16, 0, {0x06, 0xED, 0xA0, 0xA0, 0x08, 0xEF, 0xA0, 0xA0}},
    {0xEB, 7, 0, {0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00}},
    {0xED, 16, 0, {0xFF, 0xFF, 0xFF, 0xBA, 0x0A, 0xBF, 0x45, 0xFF, 0xFF, 0x54, 0xFB, 0xA0, 0xAB, 0xFF, 0xFF, 0xFF}},
    {0xEF, 6, 0, {0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F}},

    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x13}},
    {0xEF, 1, 0, {0x08}},

    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x00}},
    {0x36, 1, 0, {0x00}},          /* MADCTL: normal scan, RGB order */
    {0x3A, 1, 0, {0x66}},          /* 18-bit bus (LSBs tied to GND) */
    {0x11, 0, 480, {0}},           /* Sleep out */
    {0x20, 0, 120, {0}},           /* Inversion off (use 0x21 if colors are inverted) */
    {0x29, 0, 20, {0}},            /* Display on */
};

/* ---- 3-wire SPI, 9 bits a word (D/C, then 8 data bits), bit-banged ----------
 * Slow on purpose: 1 us a half-bit is ~0.5 Mbit/s, and the whole table is a few
 * hundred words, sent once. */

static inline void spi_bit(int level)
{
    gpio_set_level(PIN_LCD_SPI_SCK, 0);
    gpio_set_level(PIN_LCD_SPI_SDA, level);
    esp_rom_delay_us(1);
    gpio_set_level(PIN_LCD_SPI_SCK, 1);   /* the panel samples on the rising edge */
    esp_rom_delay_us(1);
}

static void spi_write9(int dc, uint8_t v)
{
    spi_bit(dc);
    for (int i = 7; i >= 0; i--) spi_bit((v >> i) & 1);
}

static void lcd_send(const lcd_cmd_t *c)
{
    gpio_set_level(PIN_LCD_SPI_CS, 0);
    spi_write9(0, c->cmd);
    for (int i = 0; i < c->len; i++) spi_write9(1, c->data[i]);
    gpio_set_level(PIN_LCD_SPI_CS, 1);
    if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
}

esp_err_t st7701_start(esp_lcd_panel_handle_t *out)
{
    /* SPI first, while GPIO 1 and 2 are still plain GPIOs: the RGB driver takes them
     * as B1/B2 the moment it is created. */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_LCD_SPI_CS) | (1ULL << PIN_LCD_SPI_SCK) |
                        (1ULL << PIN_LCD_SPI_SDA) | (1ULL << PIN_LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(PIN_LCD_SPI_CS, 1);
    gpio_set_level(PIN_LCD_SPI_SCK, 1);
    gpio_set_level(PIN_LCD_SPI_SDA, 1);

    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    for (size_t i = 0; i < sizeof(init_cmds) / sizeof(init_cmds[0]); i++) lcd_send(&init_cmds[i]);

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,
            .h_res = ST7701_W,
            .v_res = ST7701_H,
            .hsync_pulse_width = 8,
            .hsync_back_porch = 10,
            .hsync_front_porch = 50,
            .vsync_pulse_width = 3,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = 0,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        /* The frame buffer is 450 KB and lives in PSRAM; the LCD DMA reads internal
         * bounce buffers that the CPU refills from it, ten lines at a time (2 x 9.6 KB
         * of internal RAM). Without them a busy PSRAM bus — Wi-Fi, TLS, a flash write —
         * starves the DMA and the picture drifts sideways. */
        .bounce_buffer_size_px = ST7701_W * 10,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
        .dma_burst_size = 64,
#else
        .psram_trans_align = 64,
#endif
        .hsync_gpio_num = PIN_LCD_HSYNC,
        .vsync_gpio_num = PIN_LCD_VSYNC,
        .de_gpio_num = PIN_LCD_DE,
        .pclk_gpio_num = PIN_LCD_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = PIN_LCD_DATA,
        .flags.fb_in_psram = 1,
    };
    esp_lcd_panel_handle_t panel = NULL;
    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_rgb_panel: %s", esp_err_to_name(err));
        return err;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));   /* an RGB panel's reset is a no-op */
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));    /* starts the stream */
    *out = panel;
    ESP_LOGI(TAG, "%dx%d RGB stream up", ST7701_W, ST7701_H);
    return ESP_OK;
}

void st7701_set_on(bool on)
{
    static const lcd_cmd_t off[] = {
        {0x28, 0, 20, {0}},     /* DISPOFF */
        {0x10, 0, 120, {0}},    /* SLPIN */
    };
    static const lcd_cmd_t wake[] = {
        {0x11, 0, 120, {0}},    /* SLPOUT */
        {0x29, 0, 20, {0}},     /* DISPON */
    };

    /* Take SCK and SDA back from the LCD peripheral (they are data0 and data1 of the
     * bus), send, then hand them back. The pins stay outputs throughout. */
    esp_rom_gpio_connect_out_signal(PIN_LCD_SPI_SCK, SIG_GPIO_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(PIN_LCD_SPI_SDA, SIG_GPIO_OUT_IDX, false, false);
    const lcd_cmd_t *seq = on ? wake : off;
    for (int i = 0; i < 2; i++) lcd_send(&seq[i]);
    esp_rom_gpio_connect_out_signal(PIN_LCD_SPI_SCK, LCD_DATA_OUT0_IDX, false, false);
    esp_rom_gpio_connect_out_signal(PIN_LCD_SPI_SDA, LCD_DATA_OUT1_IDX, false, false);
}
