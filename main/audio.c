#include "audio.h"
#include "board_pins.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "audio";

/* Wiring lives in board_pins.h. The amplifier and the microphone share one bit clock
 * and one word clock, so both directions are the two halves of ONE I2S controller in
 * full duplex. Two controllers — what this file used to run — would each drive their
 * own clock onto the same wire.
 *
 * Sharing a clock means sharing a slot width, and the microphone dictates it: the
 * INMP441 is a 24-bit part that wants 32-bit slots. So the speaker side is 32-bit too,
 * and audio_write() widens each 16-bit sample into the top of a 32-bit slot, which the
 * MAX98357A reads as the same value. */
#define BCLK      PIN_I2S_BCLK
#define WS        PIN_I2S_WS
#define SPK_DIN   PIN_SPK_DIN
#define MIC_SD    PIN_MIC_SD

static i2s_chan_handle_t s_tx, s_rx;
/* Which half of the I2S frame carries the microphone. The INMP441 drives SD only
 * during the half its LR pin selects and leaves it floating during the other — so
 * reading the wrong half returns a line that is not being driven, which looks like a
 * steady noise floor that ignores every sound in the room. That is exactly what the
 * first live test showed. Both halves are captured and this picks one. LR is tied to
 * GND on this board, so it is the left. */
static int s_mic_slot = 0;              /* 0 = left (WS low), 1 = right */

void audio_speaker_enable(bool on)
{
    /* Nothing to switch: SD is strapped to VIN through 100 k on this board, which is
     * "on, left channel". The calls stay where a muting pin would be driven, so a board
     * that brings SD back to a GPIO needs this function and nothing else. Between
     * utterances the TX side sends zeros (auto_clear), which is quiet but not silent. */
    (void)on;
}

bool audio_start(void)
{
    if (s_tx && s_rx) return true;

    i2s_chan_config_t cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* Silence on underrun, not the last buffer again. Left at its default, the DMA
     * replays whatever it last held whenever the voice stream runs dry for a moment
     * — a Wi-Fi hiccup becomes a stuttering repeat of the Buddy's last syllable.
     * (It only affects the TX side.) */
    cfg.auto_clear = true;
    /* Six 20 ms buffers: 120 ms of cover for a packet that arrives late. The default
     * (6 x 240 frames) is 90 ms, less than the 100 ms the player waits to rebuild its
     * cushion after a real underrun. In full duplex the RX side gets the same. */
    cfg.dma_desc_num = 6;
    cfg.dma_frame_num = AUDIO_SAMPLE_RATE / 50;
    if (i2s_new_channel(&cfg, &s_tx, &s_rx) != ESP_OK) { ESP_LOGE(TAG, "no I2S channels"); return false; }

    /* Speaker: 32-bit mono, on the left slot — the half the amp's SD strap selects. */
    i2s_std_config_t tx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = BCLK, .ws = WS,
                      .dout = SPK_DIN, .din = I2S_GPIO_UNUSED },
    };
    tx_std.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &tx_std));

    /* Microphone: both halves captured, see s_mic_slot. Asking the INMP441 for 16 bits
     * gets silence or noise depending on the day; the samples are narrowed in
     * audio_read() instead, where the shift is visible. */
    i2s_std_config_t rx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = BCLK, .ws = WS,
                      .dout = I2S_GPIO_UNUSED, .din = MIC_SD },
    };
    rx_std.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &rx_std));

    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));

    ESP_LOGI(TAG, "I2S0 full duplex: BCLK %d, WS %d, speaker DIN %d, mic SD %d, %d Hz",
             BCLK, WS, SPK_DIN, MIC_SD, AUDIO_SAMPLE_RATE);
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
    /* Widened in 64-sample bites, into the top half of each 32-bit slot. The
     * MAX98357A takes the most significant bits first and ignores what it has no use
     * for, so a 16-bit sample << 16 plays at exactly the level it did as 16 bits. */
    int32_t wide[64];
    size_t done = 0;
    while (done < count) {
        size_t n = count - done < 64 ? count - done : 64;
        for (size_t i = 0; i < n; i++) wide[i] = (int32_t)samples[done + i] << 16;
        size_t written = 0;
        if (i2s_channel_write(s_tx, wide, n * sizeof(int32_t), &written, 1000) != ESP_OK) break;
        done += written / sizeof(int32_t);
        if (written < n * sizeof(int32_t)) break;
    }
    return done;
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
        ESP_LOGE(TAG, "mic reads digital silence — check SD->%d, and that L/R is tied to GND", MIC_SD);

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
