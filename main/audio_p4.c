#include "audio.h"
#include "board_i2c.h"
#include "board_pins.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include <math.h>
#include <string.h>

static const char *TAG = "audio";

/* The Waveshare ESP32-P4-WIFI6-Touch-LCD-4C, wired as its BSP has it: one I2S controller
 * whose TX half feeds the ES8311 (DAC, then the speaker amplifier on PA_EN) and whose RX
 * half reads the ES7210 (four-channel ADC, two microphones on it). Both chips are slaves
 * on the shared clocks, and both are configured over the board's one I2C bus.
 *
 * One clock means one frame length. The ES7210 sends four 16-bit slots (TDM, 64 bit
 * clocks a frame); the ES8311 is opened as 16-bit stereo, which esp_codec_dev widens to
 * 32-bit slots so that its frame is 64 bit clocks as well. Its log says so ("has
 * extended slot bits from 32 to 64"), and a speaker that plays at double speed means it
 * did not.
 *
 * esp_codec_dev enables the I2S channels itself, so they are initialised here and left
 * disabled until the codecs are opened. */

#define DMA_DESC   6
#define DMA_FRAMES (AUDIO_SAMPLE_RATE / 50)   /* 20 ms: 120 ms of playback cover, as on the S3 */

static i2s_chan_handle_t s_tx, s_rx;
static esp_codec_dev_handle_t s_spk, s_mic;
static int s_mic_slot;                         /* the first 'M' in the format */
static bool s_spk_muted = true;
static int s_vol = CONFIG_BUDDY_SPEAKER_VOLUME;
static int s_vol_saved = -1;            /* what NVS holds, to not rewrite it */


/* ---- the software echo reference ----------------------------------------------
 *
 * For a board whose ES7210 has no loopback slot. Whatever audio_write() hands to the
 * DMA is kept here and read back by the microphone task in step with the microphone.
 * What audio_write() queues is heard about one TX DMA queue later, so when a burst
 * starts the ring is first padded with that much silence; the reference then leads the
 * echo by a few milliseconds, which the canceller's filter covers, rather than lagging
 * it, which it cannot. */
#define REF_CAP AUDIO_SAMPLE_RATE              /* one second */
static int16_t *s_ref;
static size_t s_ref_head, s_ref_tail, s_ref_fill;
static bool s_ref_on, s_ref_idle = true;
static portMUX_TYPE s_ref_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_ref_dropped;

static void ref_push(const int16_t *pcm, size_t n, bool pad)
{
    size_t lost = 0;
    portENTER_CRITICAL(&s_ref_lock);
    size_t lead = pad && s_ref_idle ? (size_t)DMA_DESC * DMA_FRAMES : 0;
    s_ref_idle = false;
    for (size_t i = 0; i < lead + n; i++) {
        if (s_ref_fill == REF_CAP) { lost++; continue; }
        s_ref[s_ref_head] = i < lead ? 0 : pcm[i - lead];
        s_ref_head = (s_ref_head + 1) % REF_CAP;
        s_ref_fill++;
    }
    s_ref_dropped += lost;
    portEXIT_CRITICAL(&s_ref_lock);
}

void audio_ref_enable(bool on)
{
    if (on && !s_ref) {
        s_ref = heap_caps_malloc(REF_CAP * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!s_ref) { ESP_LOGE(TAG, "no memory for the echo reference"); return; }
    }
    portENTER_CRITICAL(&s_ref_lock);
    s_ref_head = s_ref_tail = s_ref_fill = 0;
    s_ref_idle = true;
    s_ref_on = on;
    portEXIT_CRITICAL(&s_ref_lock);
}

size_t audio_ref_read(int16_t *out, size_t count)
{
    size_t got = 0;
    uint32_t dropped = 0;
    portENTER_CRITICAL(&s_ref_lock);
    if (s_ref_on) {
        while (got < count && s_ref_fill) {
            out[got++] = s_ref[s_ref_tail];
            s_ref_tail = (s_ref_tail + 1) % REF_CAP;
            s_ref_fill--;
        }
        /* Ran dry: playback has stopped, or stalled long enough that the DMA is playing
         * its own silence. Either way the next burst starts a DMA queue late again. */
        if (!s_ref_fill) s_ref_idle = true;
    }
    dropped = s_ref_dropped;
    s_ref_dropped = 0;
    portEXIT_CRITICAL(&s_ref_lock);
    if (dropped) ESP_LOGW(TAG, "echo reference overflowed, %lu samples lost — is the mic task stalled?",
                          (unsigned long)dropped);
    memset(out + got, 0, (count - got) * sizeof(int16_t));
    return count;
}

/* ---- the codecs ---------------------------------------------------------------- */

void audio_speaker_enable(bool on)
{
    /* The ES8311 mutes in its DAC; the amplifier stays powered, because switching PA_EN
     * on every utterance is a click every utterance. */
    if (!s_spk || s_spk_muted == !on) return;
    s_spk_muted = !on;
    esp_codec_dev_set_out_mute(s_spk, !on);
}

/* The chosen volume outlives a reboot: a parent who turned it down at bedtime did
 * not mean "until the next power cut". BUDDY_SPEAKER_VOLUME is only the value a kit
 * starts life with, and stays the fallback if NVS has nothing. */
#define VOL_NS   "buddy"
#define VOL_KEY  "volume"

static int volume_restore(void)
{
    nvs_handle_t h;
    int32_t v = CONFIG_BUDDY_SPEAKER_VOLUME;
    if (nvs_open(VOL_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, VOL_KEY, &v) != ESP_OK || v < 0 || v > 100)
            v = CONFIG_BUDDY_SPEAKER_VOLUME;
        nvs_close(h);
    }
    return (int)v;
}

static void volume_save(int percent)
{
    nvs_handle_t h;
    if (nvs_open(VOL_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_i32(h, VOL_KEY, percent) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static int first_slot(char kind)
{
    const char *f = CONFIG_BUDDY_AFE_INPUT_FORMAT;
    for (int i = 0; i < AUDIO_IN_SLOTS && f[i]; i++) if (f[i] == kind) return i;
    return -1;
}

bool audio_start(void)
{
    if (s_spk && s_mic) return true;

    if (strlen(CONFIG_BUDDY_AFE_INPUT_FORMAT) != AUDIO_IN_SLOTS)
        ESP_LOGW(TAG, "BUDDY_AFE_INPUT_FORMAT \"%s\" should name %d slots",
                 CONFIG_BUDDY_AFE_INPUT_FORMAT, AUDIO_IN_SLOTS);
    s_mic_slot = first_slot('M');
    if (s_mic_slot < 0) {
        ESP_LOGE(TAG, "BUDDY_AFE_INPUT_FORMAT \"%s\" has no microphone; using slot 0",
                 CONFIG_BUDDY_AFE_INPUT_FORMAT);
        s_mic_slot = 0;
    }

    i2c_master_bus_handle_t bus = board_i2c();
    if (!bus) return false;

    i2s_chan_config_t cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cfg.auto_clear = true;                 /* silence on underrun, not a stutter */
    cfg.dma_desc_num = DMA_DESC;
    cfg.dma_frame_num = DMA_FRAMES;
    if (i2s_new_channel(&cfg, &s_tx, &s_rx) != ESP_OK) { ESP_LOGE(TAG, "no I2S channels"); return false; }

    /* Initial modes only — esp_codec_dev reconfigures slots and clocks on open. What
     * matters here is the mode of each half and the pins. */
    i2s_std_config_t tx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = PIN_I2S_MCLK, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_WS,
                      .dout = PIN_I2S_DOUT, .din = PIN_I2S_DIN },
    };
    tx_std.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    i2s_tdm_config_t rx_tdm = {
        .clk_cfg  = I2S_TDM_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = { .mclk = PIN_I2S_MCLK, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_WS,
                      .dout = PIN_I2S_DOUT, .din = PIN_I2S_DIN },
    };
    if (i2s_channel_init_std_mode(s_tx, &tx_std) != ESP_OK ||
        i2s_channel_init_tdm_mode(s_rx, &rx_tdm) != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed");
        return false;
    }

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = s_rx, .tx_handle = s_tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    /* ES7210 first: it sets the frame (4 x 16 bits), and the ES8311 is fitted to it. */
    audio_codec_i2c_cfg_t adc_i2c = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    es7210_codec_cfg_t adc_cfg = {
        .ctrl_if = adc_ctrl,
        /* All four, which is what puts the ES7210 in TDM: with two selected it sends a
         * standard stereo frame and the slot layout below means nothing. */
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *adc_if = adc_ctrl ? es7210_codec_new(&adc_cfg) : NULL;
    esp_codec_dev_cfg_t mic_dev = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc_if, .data_if = data_if };
    s_mic = adc_if ? esp_codec_dev_new(&mic_dev) : NULL;
    esp_codec_dev_sample_info_t in_fs = {
        .bits_per_sample = 16, .channel = AUDIO_IN_SLOTS, .channel_mask = 0xF,
        .sample_rate = AUDIO_SAMPLE_RATE, .mclk_multiple = 256,
    };
    if (!s_mic || esp_codec_dev_open(s_mic, &in_fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "ES7210 did not answer at 0x%02x (I2C SDA %d / SCL %d)",
                 ES7210_CODEC_DEFAULT_ADDR >> 1, PIN_I2C_SDA, PIN_I2C_SCL);
        s_mic = NULL;
        return false;
    }
    for (int i = 0; i < AUDIO_IN_SLOTS; i++) {
        float db = CONFIG_BUDDY_AFE_INPUT_FORMAT[i] == 'R' ? 0 : CONFIG_BUDDY_MIC_GAIN_DB;
        esp_codec_dev_set_in_channel_gain(s_mic, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(i), db);
    }

    audio_codec_i2c_cfg_t dac_i2c = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    es8311_codec_cfg_t dac_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PIN_PA_EN,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac_if = dac_ctrl ? es8311_codec_new(&dac_cfg) : NULL;
    esp_codec_dev_cfg_t spk_dev = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac_if, .data_if = data_if };
    s_spk = dac_if ? esp_codec_dev_new(&spk_dev) : NULL;
    /* Stereo, not mono: the library sizes the widened slot as frame bits / channels, and
     * 64 / 1 is not a slot width I2S has. audio_write() duplicates each sample. */
    esp_codec_dev_sample_info_t out_fs = {
        .bits_per_sample = 16, .channel = 2, .sample_rate = AUDIO_SAMPLE_RATE, .mclk_multiple = 256,
    };
    if (!s_spk || esp_codec_dev_open(s_spk, &out_fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "ES8311 did not answer at 0x%02x", ES8311_CODEC_DEFAULT_ADDR >> 1);
        s_spk = NULL;
        return false;
    }
    s_vol = s_vol_saved = volume_restore();
    esp_codec_dev_set_out_vol(s_spk, s_vol);
    s_spk_muted = false;

    ESP_LOGI(TAG, "ES8311 out + ES7210 in on I2S0, %d Hz; slots \"%s\", mic %d dB, volume %d%%%s",
             AUDIO_SAMPLE_RATE, CONFIG_BUDDY_AFE_INPUT_FORMAT, CONFIG_BUDDY_MIC_GAIN_DB, s_vol,
             s_vol == CONFIG_BUDDY_SPEAKER_VOLUME ? "" : " (kept from last time)");
    return true;
}

void audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent == s_vol) return;
    s_vol = percent;
    /* The codec takes it immediately; a buffer already inside the DMA plays at the
     * old level, which is 20 ms and nobody hears the seam. Flash is not touched here
     * — see audio_volume_persist(). */
    if (s_spk) esp_codec_dev_set_out_vol(s_spk, s_vol);
}

int audio_get_volume(void) { return s_vol; }

void audio_volume_persist(void)
{
    if (s_vol == s_vol_saved) return;
    s_vol_saved = s_vol;
    volume_save(s_vol);
    ESP_LOGI(TAG, "volume %d%%", s_vol);
}

size_t audio_read_slots(int16_t *frames, size_t count)
{
    if (!s_mic) return 0;
    if (esp_codec_dev_read(s_mic, frames, count * AUDIO_IN_SLOTS * sizeof(int16_t)) != ESP_CODEC_DEV_OK)
        return 0;
    return count;
}

size_t audio_read(int16_t *samples, size_t count)
{
    /* 64 frames at a time, so the scratch stays on the stack. */
    int16_t wide[64 * AUDIO_IN_SLOTS];
    size_t done = 0;
    while (done < count) {
        size_t want = count - done < 64 ? count - done : 64;
        if (!audio_read_slots(wide, want)) break;
        for (size_t i = 0; i < want; i++) samples[done + i] = wide[i * AUDIO_IN_SLOTS + s_mic_slot];
        done += want;
    }
    return done;
}

size_t audio_write(const int16_t *samples, size_t count)
{
    if (!s_spk) return 0;
    int16_t pair[128];
    size_t done = 0;
    while (done < count) {
        size_t n = count - done < 64 ? count - done : 64;
        for (size_t i = 0; i < n; i++) pair[2 * i] = pair[2 * i + 1] = samples[done + i];
        /* Into the reference before the DMA, so the reference is never behind. */
        if (s_ref_on) ref_push(samples + done, n, true);
        if (esp_codec_dev_write(s_spk, pair, n * 2 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) break;
        done += n;
    }
    return done;
}

/* ---- self-test ----------------------------------------------------------------- */

typedef struct { double sum[AUDIO_IN_SLOTS], mean[AUDIO_IN_SLOTS]; int32_t peak[AUDIO_IN_SLOTS]; size_t n; } levels_t;

static void levels_add(levels_t *l, const int16_t *f, size_t frames)
{
    for (size_t i = 0; i < frames; i++)
        for (int s = 0; s < AUDIO_IN_SLOTS; s++) {
            int v = f[i * AUDIO_IN_SLOTS + s];
            l->mean[s] += v;
            l->sum[s] += (double)v * v;
            int a = v < 0 ? -v : v;
            if (a > l->peak[s]) l->peak[s] = a;
        }
    l->n += frames;
}

static double levels_db(const levels_t *l, int s)
{
    if (!l->n) return -99;
    double m = l->mean[s] / l->n;
    double var = l->sum[s] / l->n - m * m;      /* DC out: a level is sound, not offset */
    return var > 0 ? 10.0 * log10(var / (32768.0 * 32768.0)) : -99;
}

static void bar(char *out, double db)
{
    int b = (int)((db + 70.0) * 12.0 / 70.0);   /* -70..0 dBFS across 12 characters */
    b = b < 0 ? 0 : b > 12 ? 12 : b;
    memset(out, '#', b);
    memset(out + b, ' ', 12 - b);
    out[12] = 0;
}

void audio_selftest(void)
{
    if (!s_spk || !s_mic) return;
    static int16_t f[DMA_FRAMES * AUDIO_IN_SLOTS];
    const char *fmt = CONFIG_BUDDY_AFE_INPUT_FORMAT;

    /* Let the ADC and the microphones settle; the first reads are their switch-on. */
    for (int i = 0; i < 10; i++) audio_read_slots(f, DMA_FRAMES);

    levels_t quiet = { 0 }, tone = { 0 };
    for (int i = 0; i < 10; i++) { audio_read_slots(f, DMA_FRAMES); levels_add(&quiet, f, DMA_FRAMES); }

    /* A quiet A for 400 ms, written and read in step, 20 ms at a time. The writes
     * run a DMA queue ahead of the speaker, so the first reads are still quiet; the
     * second half of the tone is what the slots are judged on. */
    ESP_LOGI(TAG, "playing 440 Hz for 400 ms");
    int16_t chunk[DMA_FRAMES];
    for (int c = 0; c < 20; c++) {
        for (int i = 0; i < DMA_FRAMES; i++)
            chunk[i] = (int16_t)(6000.0f * sinf(2.0f * (float)M_PI * 440.0f * (c * DMA_FRAMES + i) / AUDIO_SAMPLE_RATE));
        audio_write(chunk, DMA_FRAMES);
        audio_read_slots(f, DMA_FRAMES);
        if (c >= 10) levels_add(&tone, f, DMA_FRAMES);
    }
    memset(chunk, 0, sizeof(chunk));
    audio_write(chunk, DMA_FRAMES);

    int loud = -1;
    double best = -99;
    for (int s = 0; s < AUDIO_IN_SLOTS; s++) {
        double q = levels_db(&quiet, s), t = levels_db(&tone, s);
        ESP_LOGI(TAG, "slot %d ('%c'): quiet %5.1f dBFS, tone %5.1f dBFS (peak %ld)%s", s,
                 fmt[s] ? fmt[s] : '?', q, t, (long)tone.peak[s],
                 !quiet.peak[s] && !tone.peak[s] ? "  — digital silence" : "");
        if (t - q > best) { best = t - q; loud = s; }
    }
    /* A loopback slot rises the most under the tone, because it hears the DAC directly
     * and the microphones only hear the speaker through air and plastic. */
    if (loud >= 0 && first_slot('R') >= 0 && loud != first_slot('R'))
        ESP_LOGW(TAG, "slot %d rose most under the tone, but the format puts R at slot %d — "
                 "check BUDDY_AFE_INPUT_FORMAT", loud, first_slot('R'));

    ESP_LOGI(TAG, "LIVE LEVEL for 10 s, each slot — quiet, then talk, then clap");
    for (int w = 0; w < 50; w++) {
        levels_t l = { 0 };
        for (int i = 0; i < 10; i++) { audio_read_slots(f, DMA_FRAMES); levels_add(&l, f, DMA_FRAMES); }
        char b[AUDIO_IN_SLOTS][13];
        double d[AUDIO_IN_SLOTS];
        for (int s = 0; s < AUDIO_IN_SLOTS; s++) { d[s] = levels_db(&l, s); bar(b[s], d[s]); }
        ESP_LOGI(TAG, "0 %5.1f |%s| 1 %5.1f |%s| 2 %5.1f |%s| 3 %5.1f |%s|",
                 d[0], b[0], d[1], b[1], d[2], b[2], d[3], b[3]);
    }
    ESP_LOGI(TAG, "self-test done");
}
