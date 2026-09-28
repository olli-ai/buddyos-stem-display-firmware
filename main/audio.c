#include "audio.h"
#include "board_pins.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "audio";

/* Wiring lives in board_pins.h. The two `SD` pins are unrelated despite sharing a
 * name: on the amplifier it is shutdown (driven high to enable), on the microphone
 * it is the data line (an input to us). Getting those the same way round is the
 * wiring mistake this comment exists to prevent. */
#define SPK_SD    PIN_SPK_SD
#define SPK_GAIN  PIN_SPK_GAIN
#define SPK_DIN   PIN_SPK_DIN
#define SPK_BCLK  PIN_SPK_BCLK
#define SPK_LRC   PIN_SPK_LRC

#define MIC_SD    PIN_MIC_SD
#define MIC_SCK   PIN_MIC_SCK
#define MIC_WS    PIN_MIC_WS
#define MIC_LR    PIN_MIC_LR

static i2s_chan_handle_t s_tx, s_rx;
/* Which half of the I2S frame carries the microphone. The INMP441 drives SD only
 * during the half its LR pin selects and leaves it floating during the other — so
 * reading the wrong half returns a line that is not being driven, which looks like a
 * steady noise floor that ignores every sound in the room. That is exactly what the
 * first live test showed. Both halves are captured and this picks one. */
static int s_mic_slot = 0;              /* 0 = left (WS low), 1 = right */

void audio_speaker_enable(bool on)
{
    gpio_set_level(SPK_SD, on ? 1 : 0);
}

void audio_set_gain(audio_gain_t gain)
{
    /* The MAX98357A reads its GAIN pin as a level, not a signal: tied to ground it
     * is 12 dB, left floating it is 9 dB. Tying it to VDD would give 6 dB — but the
     * amplifier runs from 5 V here and a 3.3 V GPIO is not a trustworthy "VDD", so
     * that level is simply not offered. Floating is the default: the quietest of the
     * two, for a device that may sit beside a bed. */
    if (gain == AUDIO_GAIN_LOUD) {
        gpio_set_direction(SPK_GAIN, GPIO_MODE_OUTPUT);
        gpio_set_level(SPK_GAIN, 0);
    } else {
        gpio_set_direction(SPK_GAIN, GPIO_MODE_INPUT);
        gpio_set_pull_mode(SPK_GAIN, GPIO_FLOATING);
    }
}

bool audio_start(void)
{
    if (s_tx && s_rx) return true;

    gpio_config_t en = { .pin_bit_mask = 1ULL << SPK_SD, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&en);
    audio_speaker_enable(false);
    audio_set_gain(AUDIO_GAIN_QUIET);

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
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = MIC_SCK, .ws = MIC_WS,
                      .dout = I2S_GPIO_UNUSED, .din = MIC_SD },
    };
    rx_std.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &rx_std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));

    ESP_LOGI(TAG, "mic on %d/%d/%d, speaker on %d/%d/%d, %d Hz mono",
             MIC_SCK, MIC_WS, MIC_SD, SPK_BCLK, SPK_LRC, SPK_DIN, AUDIO_SAMPLE_RATE);
    return true;
}

size_t audio_read(int16_t *samples, size_t count)
{
    if (!s_rx) return 0;
    /* Stereo frames in 32-frame bites, so the scratch stays on the stack. */
    int32_t wide[64];
    size_t done = 0;
    while (done < count) {
        size_t want = count - done < 32 ? count - done : 32;
        size_t got = 0;
        if (i2s_channel_read(s_rx, wide, want * 2 * sizeof(int32_t), &got, 50) != ESP_OK) break;
        got /= 2 * sizeof(int32_t);
        if (!got) break;
        /* 24 bits left-justified in 32: >> 16 keeps the top 16 and the sign. */
        for (size_t i = 0; i < got; i++) samples[done + i] = (int16_t)(wide[2 * i + s_mic_slot] >> 16);
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

    /* Listen first. If driving the amplifier disturbs anything — and on a board
     * already running a panel and a radio from one USB port, it might — the
     * microphone's answer is already in the log rather than lost along with it.
     *
     * A fifth of a second. Digital silence, every sample exactly zero, means the data
     * line is not connected; a quiet room still reads about -60 dBFS because
     * microphones have noise floors. */
    static int16_t heard[AUDIO_SAMPLE_RATE / 5];

    /* Let it wake up first. The INMP441 outputs zeros for its first 2^18 clock
     * cycles — about 85 ms here — and then its filters settle with a transient
     * large enough to read as near full scale. The first version of this test
     * started listening the moment I2S was enabled and reported -3.9 dBFS for a
     * silent room: it was measuring the microphone switching on. */
    for (int i = 0; i < 3; i++) audio_read(heard, sizeof(heard) / sizeof(heard[0]));

    size_t got = audio_read(heard, sizeof(heard) / sizeof(heard[0]));

    /* And take the DC offset out before calling anything a level. A MEMS mic sits
     * a little off zero, and RMS would count that offset as sound. */
    double mean = 0;
    for (size_t i = 0; i < got; i++) mean += heard[i];
    mean = got ? mean / got : 0;
    double sum = 0;
    int32_t peak = 0, nonzero = 0;
    for (size_t i = 0; i < got; i++) {
        double v = heard[i] - mean;
        sum += v * v;
        int32_t a = (int32_t)(v < 0 ? -v : v);
        if (a > peak) peak = a;
        if (heard[i]) nonzero++;
    }
    double rms = got ? sqrt(sum / got) : 0;
    double dbfs = rms > 0 ? 20.0 * log10(rms / 32768.0) : -999;
    ESP_LOGI(TAG, "mic: %u samples, %.1f dBFS (DC %+.0f), peak %ld, %d%% non-zero",
             (unsigned)got, dbfs, mean, (long)peak, got ? (int)(100 * nonzero / got) : 0);
    if (!nonzero)
        ESP_LOGE(TAG, "mic reads digital silence — check SD->%d, and that LR is tied", MIC_SD);

    /* Then a short, quiet A: loud enough to hear across a room, short enough not to
     * become the sound this device is remembered for. */
    static int16_t tone[AUDIO_SAMPLE_RATE / 5];
    for (int i = 0; i < (int)(sizeof(tone) / sizeof(tone[0])); i++)
        tone[i] = (int16_t)(6000.0f * sinf(2.0f * (float)M_PI * 440.0f * i / AUDIO_SAMPLE_RATE));
    ESP_LOGI(TAG, "playing 440 Hz for 200 ms");
    audio_speaker_enable(true);
    audio_write(tone, sizeof(tone) / sizeof(tone[0]));
    audio_speaker_enable(false);
    /* Then a live meter, because one number cannot tell sound from hum. A level
     * that moves when someone talks is a microphone hearing a room; a level that
     * sits still whatever happens is interference, and the fix for that is a clean
     * supply rather than code. The beep above is the cue: quiet, then talk, then
     * clap, and the three should look nothing alike.
     *
     * 200 ms windows reuse the buffer above, so this costs no memory. */
    ESP_LOGI(TAG, "LIVE LEVEL for 10 s, LEFT and RIGHT halves — quiet, then talk, then clap");
    for (int w = 0; w < 50; w++) {
        double db[2]; long pkv[2]; char bar[2][21];
        for (int ch = 0; ch < 2; ch++) {
            s_mic_slot = ch;
            size_t n = audio_read(heard, sizeof(heard) / sizeof(heard[0]) / 2);
            double m = 0;
            for (size_t i = 0; i < n; i++) m += heard[i];
            m = n ? m / n : 0;
            double acc = 0;
            int32_t pk = 0;
            for (size_t i = 0; i < n; i++) {
                double v = heard[i] - m;
                acc += v * v;
                int32_t a = (int32_t)(v < 0 ? -v : v);
                if (a > pk) pk = a;
            }
            double r = n ? sqrt(acc / n) : 0;
            db[ch] = r > 0 ? 20.0 * log10(r / 32768.0) : -99;
            pkv[ch] = pk;
            /* A bar across -70..0 dBFS, so a change is visible without reading digits. */
            int b = (int)((db[ch] + 70.0) * 20.0 / 70.0);
            if (b < 0) b = 0;
            if (b > 20) b = 20;
            memset(bar[ch], '#', b);
            memset(bar[ch] + b, ' ', 20 - b);
            bar[ch][20] = 0;
        }
        ESP_LOGI(TAG, "L %5.1f pk %5ld |%s|   R %5.1f pk %5ld |%s|",
                 db[0], pkv[0], bar[0], db[1], pkv[1], bar[1]);
    }
    s_mic_slot = 0;
    ESP_LOGI(TAG, "self-test done");
}
