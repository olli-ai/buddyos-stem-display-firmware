#include "audio.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "audio";

/* Wiring, as built on the bench 2026-09-28.
 *
 * MAX98357A  SD->5  DIN->4  BCLK->6  LRC->7
 * INMP441    SD->17 SCK->16 WS->3    LR->18
 *
 * The two `SD` pins are unrelated despite the name: on the amplifier it is
 * shutdown/gain-mode (driven high here to enable), on the microphone it is the data
 * line (an input to us). Getting those the same way round is the wiring mistake this
 * comment exists to prevent. */
#define SPK_SD    5
#define SPK_DIN   4
#define SPK_BCLK  6
#define SPK_LRC   7

#define MIC_SD    17
#define MIC_SCK   16
#define MIC_WS    3
#define MIC_LR    18

static i2s_chan_handle_t s_tx, s_rx;

void audio_speaker_enable(bool on)
{
    gpio_set_level(SPK_SD, on ? 1 : 0);
}

bool audio_start(void)
{
    if (s_tx && s_rx) return true;

    gpio_config_t en = { .pin_bit_mask = 1ULL << SPK_SD, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&en);
    audio_speaker_enable(false);

    /* The INMP441's L/R pin picks which half of the frame it drives. Tied low here
     * and read as the left channel; left floating it is neither, and the symptom is
     * silence that looks exactly like a dead microphone. */
    gpio_config_t lr = { .pin_bit_mask = 1ULL << MIC_LR, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&lr);
    gpio_set_level(MIC_LR, 0);

    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&tx_cfg, &s_tx, NULL) != ESP_OK) { ESP_LOGE(TAG, "no tx channel"); return false; }
    i2s_std_config_t tx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = SPK_BCLK, .ws = SPK_LRC,
                      .dout = SPK_DIN, .din = I2S_GPIO_UNUSED },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &tx_std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    /* The microphone is a 24-bit part that clocks 32-bit slots. Asking for 16 bits
     * here gets silence or noise depending on the day; the samples are narrowed in
     * audio_read() instead, where the shift is visible. */
    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    if (i2s_new_channel(&rx_cfg, NULL, &s_rx) != ESP_OK) { ESP_LOGE(TAG, "no rx channel"); return false; }
    i2s_std_config_t rx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = MIC_SCK, .ws = MIC_WS,
                      .dout = I2S_GPIO_UNUSED, .din = MIC_SD },
    };
    rx_std.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;      /* matches LR tied low */
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &rx_std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));

    ESP_LOGI(TAG, "mic on %d/%d/%d, speaker on %d/%d/%d, %d Hz mono",
             MIC_SCK, MIC_WS, MIC_SD, SPK_BCLK, SPK_LRC, SPK_DIN, AUDIO_SAMPLE_RATE);
    return true;
}

size_t audio_read(int16_t *samples, size_t count)
{
    if (!s_rx) return 0;
    /* In 32-sample bites so the scratch stays on the stack and a caller asking for a
     * whole second does not need a kilobyte of it. */
    int32_t wide[32];
    size_t done = 0;
    while (done < count) {
        size_t want = count - done < 32 ? count - done : 32;
        size_t got = 0;
        if (i2s_channel_read(s_rx, wide, want * sizeof(int32_t), &got, 200) != ESP_OK) break;
        got /= sizeof(int32_t);
        if (!got) break;
        /* 24 bits left-justified in 32: >> 16 keeps the top 16 and the sign. */
        for (size_t i = 0; i < got; i++) samples[done + i] = (int16_t)(wide[i] >> 16);
        done += got;
    }
    return done;
}

size_t audio_write(const int16_t *samples, size_t count)
{
    if (!s_tx) return 0;
    size_t written = 0;
    i2s_channel_write(s_tx, samples, count * sizeof(int16_t), &written, 1000);
    return written / sizeof(int16_t);
}

void audio_selftest(void)
{
    if (!s_tx || !s_rx) return;

    /* A short, quiet A: loud enough to hear across a room, short enough not to
     * become the sound this device is remembered for. */
    static int16_t tone[AUDIO_SAMPLE_RATE / 5];
    for (int i = 0; i < (int)(sizeof(tone) / sizeof(tone[0])); i++)
        tone[i] = (int16_t)(6000.0f * sinf(2.0f * (float)M_PI * 440.0f * i / AUDIO_SAMPLE_RATE));
    audio_speaker_enable(true);
    audio_write(tone, sizeof(tone) / sizeof(tone[0]));
    audio_speaker_enable(false);

    /* Then listen for a fifth of a second and say what came back. Digital silence —
     * every sample exactly zero — means the data line is not connected; a quiet room
     * still reads around -60 dBFS because microphones have noise floors. */
    static int16_t heard[AUDIO_SAMPLE_RATE / 5];
    size_t got = audio_read(heard, sizeof(heard) / sizeof(heard[0]));
    double sum = 0;
    int32_t peak = 0, nonzero = 0;
    for (size_t i = 0; i < got; i++) {
        sum += (double)heard[i] * heard[i];
        int32_t a = heard[i] < 0 ? -heard[i] : heard[i];
        if (a > peak) peak = a;
        if (heard[i]) nonzero++;
    }
    double rms = got ? sqrt(sum / got) : 0;
    double dbfs = rms > 0 ? 20.0 * log10(rms / 32768.0) : -999;
    ESP_LOGI(TAG, "mic: %u samples, %.1f dBFS, peak %ld, %d%% non-zero",
             (unsigned)got, dbfs, (long)peak, got ? (int)(100 * nonzero / got) : 0);
    if (!nonzero)
        ESP_LOGE(TAG, "mic reads digital silence — check SD->%d, and that LR is tied", MIC_SD);
}
