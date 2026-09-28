#include "voice.h"
#include "sdkconfig.h"

#if CONFIG_BUDDY_VOICE
#include "audio.h"
#include "face_view.h"
#include "face_lipsync.h"
#include "generated/face_assets.h"
#include "generated/voice_upsampler.h"
#include "esp_websocket_client.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#if CONFIG_BUDDY_VOICE_TAP
#include "mbedtls/base64.h"
#endif
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "voice";

/* The gateway's certificate is Let's Encrypt's, chained to ISRG Root X1 — not the
 * Amazon root the broker uses. Checked with openssl before this was written, and the
 * embedded file's fingerprint matches the one Let's Encrypt publishes. */
extern const char isrg_root_x1_pem_start[] asm("_binary_isrgrootx1_pem_start");

#define IN_SAMPLES   320                 /* 20 ms at 16 kHz: what the mic delivers */
#define UP_SAMPLES   (IN_SAMPLES * VOICE_UP_L)   /* 20 ms at 48 kHz: what omni wants */
#define PKT_MAX      512                 /* one Opus packet; ~60 B at 24 kbps */

/* The jitter buffer, in 20 ms packets.
 *
 * omni paces its output at real time — one packet every 20 ms, gapless, silence
 * included — so the player is not slower than the stream. What varies is arrival:
 * Wi-Fi and TCP retransmits deliver in clumps, and over hours the two clocks drift a
 * few ppm apart. The queue absorbs the clumps; these three numbers decide what
 * happens at its edges. */
#define JB_DEPTH     32                  /* 640 ms: the most a clump can put in hand */
#define JB_HIGH      16                  /* 320 ms: above this, shed silence to catch up */
#define JB_PREFILL    5                  /* 100 ms: in hand before playing after a gap */
/* omni's silence frames are a few bytes; speech is tens. Only used to choose what to
 * shed first, so misjudging one costs 20 ms, not a word. */
#define SILENCE_MAX   8

typedef struct { uint16_t len; uint8_t data[PKT_MAX]; } pkt_t;

static esp_websocket_client_handle_t s_ws;
static QueueHandle_t s_down;             /* omni's packets, waiting to be spoken */
static void *s_enc, *s_dec;
static uint8_t *s_enc_out;
static int s_enc_out_len;
static volatile bool s_connected;
/* While the Buddy is talking. Written by the WebSocket task, read by the audio tasks;
 * a flag rather than calls across tasks, because lip-sync keeps state that only one
 * task may touch. */
static volatile bool s_speaking;
static volatile uint32_t s_shed_silence, s_shed_old, s_underruns;

bool voice_active(void) { return s_connected; }

static void show_state(const char *name)
{
    for (int i = 0; i < face_state_count; i++)
        if (!strcmp(face_states[i].name, name)) { face_view_request_state(i); return; }
}

/* omni's vocabulary, onto the face's. */
static void on_status(const char *st)
{
    if (!strcmp(st, "listening"))             { s_speaking = false; show_state("listening"); }
    else if (!strcmp(st, "processing"))       { show_state("thinking"); }
    else if (!strcmp(st, "speaking_started")) { s_speaking = true;  show_state("speaking"); }
    else if (!strcmp(st, "speaking_ended") || !strcmp(st, "interrupted"))
                                              { s_speaking = false; show_state("listening"); }
    else if (!strcmp(st, "error"))            { s_speaking = false; show_state("error"); }
    ESP_LOGI(TAG, "omni: %s", st);
}

static void on_ws(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_connected = true;
        audio_speaker_enable(true);
        ESP_LOGI(TAG, "connected");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        s_connected = false;
        s_speaking = false;
        audio_speaker_enable(false);
        show_state("idle");
        ESP_LOGW(TAG, "disconnected");
        break;
    case WEBSOCKET_EVENT_DATA:
        /* A packet or a status fits one frame; anything split across frames is not
         * something this protocol sends, and half a packet decodes as noise. */
        if (e->payload_offset != 0 || e->data_len != e->payload_len) break;
        if (e->op_code == 0x2 && e->data_len > 0 && e->data_len <= PKT_MAX) {
            UBaseType_t held = uxQueueMessagesWaiting(s_down);
            /* Running long: shed silence first. Silence is most of what omni sends
             * between turns, so this is where the slack comes from, and nothing the
             * child would notice is lost. */
            if (held >= JB_HIGH && e->data_len <= SILENCE_MAX) { s_shed_silence++; break; }
            pkt_t p = { .len = (uint16_t)e->data_len };
            memcpy(p.data, e->data_ptr, e->data_len);
            /* Full: drop the OLDEST, not this one. Dropping new audio would leave the
             * delay where it is; dropping old audio pulls the Buddy back to now. Never
             * block the socket — late audio is worse than a gap. */
            if (xQueueSend(s_down, &p, 0) != pdTRUE) {
                pkt_t old;
                xQueueReceive(s_down, &old, 0);
                s_shed_old++;
                xQueueSend(s_down, &p, 0);
            }
        } else if (e->op_code == 0x1 && e->data_len > 0) {
            cJSON *j = cJSON_ParseWithLength(e->data_ptr, e->data_len);
            const cJSON *type = j ? cJSON_GetObjectItem(j, "type") : NULL;
            const cJSON *st = j ? cJSON_GetObjectItem(j, "status") : NULL;
            if (cJSON_IsString(type) && !strcmp(type->valuestring, "status") && cJSON_IsString(st))
                on_status(st->valuestring);
            else if (cJSON_IsString(type) && !strcmp(type->valuestring, "config"))
                ESP_LOGI(TAG, "session %.*s", e->data_len > 120 ? 120 : e->data_len, e->data_ptr);
            cJSON_Delete(j);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "websocket error");
        break;
    default:
        break;
    }
}

static inline int16_t clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

/* Mic to omni. */
static void up_task(void *arg)
{
    (void)arg;
    static int16_t in[IN_SAMPLES], up[UP_SAMPLES];
    int16_t hist[VOICE_UP_PHASE] = { 0 };
    float dc_x = 0, dc_y = 0;
    uint32_t frames = 0, sent = 0;

    for (;;) {
        size_t n = audio_read(in, IN_SAMPLES);
        if (n < IN_SAMPLES) memset(in + n, 0, (IN_SAMPLES - n) * sizeof(int16_t));

        /* Half duplex, for now. While the Buddy talks, the microphone hears the
         * speaker, and omni would take its own voice for the child's and interrupt
         * itself. Silence keeps the stream continuous — omni wants it gapless — and
         * echo cancellation is what replaces this, later. */
        if (s_speaking) memset(in, 0, sizeof(in));

        /* The INMP441's DC offset is large and wanders while its high-pass settles;
         * a one-pole blocker takes it out before it reaches the encoder. */
        for (int i = 0; i < IN_SAMPLES; i++) {
            float x = in[i], y = x - dc_x + 0.995f * dc_y;
            dc_x = x; dc_y = y;
            in[i] = clamp16((int32_t)y);
        }

        /* x3, polyphase: newest sample first, three outputs per input. The filter is
         * generated by tools/design_upsampler.py and was validated against omni's
         * recogniser before a line of this was written. */
        for (int i = 0; i < IN_SAMPLES; i++) {
            memmove(hist + 1, hist, (VOICE_UP_PHASE - 1) * sizeof(int16_t));
            hist[0] = in[i];
            for (int p = 0; p < VOICE_UP_L; p++) {
                int32_t acc = 0;
                for (int k = 0; k < VOICE_UP_PHASE; k++) acc += voice_up_phase[p][k] * hist[k];
                up[i * VOICE_UP_L + p] = clamp16((acc + (1 << 14)) >> 15);
            }
        }

        esp_audio_enc_in_frame_t fin = { .buffer = (uint8_t *)up, .len = sizeof(up) };
        esp_audio_enc_out_frame_t fout = { .buffer = s_enc_out, .len = (uint32_t)s_enc_out_len };
        frames++;
        if (esp_opus_enc_process(s_enc, &fin, &fout) != ESP_AUDIO_ERR_OK || !fout.encoded_bytes) continue;
        if (s_connected &&
            esp_websocket_client_send_bin(s_ws, (const char *)s_enc_out, fout.encoded_bytes,
                                          pdMS_TO_TICKS(50)) > 0)
            sent++;
#if CONFIG_BUDDY_VOICE_TAP
        /* The exact bytes omni was sent, while it is the child's turn. */
        if (s_connected && !s_speaking) {
            static unsigned char b64[256];
            size_t olen = 0;
            if (mbedtls_base64_encode(b64, sizeof(b64), &olen, s_enc_out, fout.encoded_bytes) == 0)
                printf("TAP %lu %.*s\n", (unsigned long)frames, (int)olen, b64);
        }
#endif
        if (frames % 500 == 0)
            ESP_LOGI(TAG, "up: %lu frames, %lu sent (%u B last)", (unsigned long)frames,
                     (unsigned long)sent, (unsigned)fout.encoded_bytes);
    }
}

/* omni to the amp, and to the mouth. */
static void down_task(void *arg)
{
    (void)arg;
    static pkt_t p;
    static int16_t pcm[960];             /* up to 60 ms at 16 kHz */
    bool was_speaking = false;

    bool refill = true;                  /* start with a cushion, not a single packet */
    uint32_t played = 0;

    for (;;) {
        /* After a gap, wait for JB_PREFILL packets before playing again. Resuming on
         * one packet in hand means the next hiccup underruns straight away, and the
         * Buddy stutters instead of pausing once. */
        if (refill) {
            while (uxQueueMessagesWaiting(s_down) < JB_PREFILL) vTaskDelay(pdMS_TO_TICKS(10));
            refill = false;
        }
        if (xQueueReceive(s_down, &p, 0) != pdTRUE) {
            s_underruns++;
            refill = true;               /* I2S plays silence meanwhile: auto_clear */
            continue;
        }
        if (++played % 1500 == 0)        /* every 30 s of playback */
            ESP_LOGI(TAG, "down: held %u, shed %lu silence + %lu old, %lu underruns",
                     (unsigned)uxQueueMessagesWaiting(s_down), (unsigned long)s_shed_silence,
                     (unsigned long)s_shed_old, (unsigned long)s_underruns);
        esp_audio_dec_in_raw_t raw = { .buffer = p.data, .len = p.len };
        esp_audio_dec_out_frame_t fo = { .buffer = (uint8_t *)pcm, .len = sizeof(pcm) };
        esp_audio_dec_info_t info;
        if (esp_opus_dec_decode(s_dec, &raw, &fo, &info) != ESP_AUDIO_ERR_OK) continue;
        size_t ns = fo.decoded_size / sizeof(int16_t);
        audio_write(pcm, ns);

        /* The same samples that go to the speaker drive the mouth, so the lips follow
         * what the child actually hears. Lip-sync state lives on this task alone. */
        bool sp = s_speaking;
        if (sp && !was_speaking) face_lipsync_start();
        if (!sp && was_speaking) { face_lipsync_stop(); face_view_set_live_mouth(-1); }
        if (sp) face_view_set_live_mouth(face_lipsync_feed(pcm, ns));
        was_speaking = sp;
    }
}

bool voice_start(const char *serial)
{
    if (s_ws) return true;
    if (!CONFIG_BUDDY_VOICE_TOKEN[0]) {
        ESP_LOGE(TAG, "no user token set (CONFIG_BUDDY_VOICE_TOKEN): omni resolves the persona with it");
        return false;
    }

    esp_opus_enc_config_t ec = ESP_OPUS_ENC_CONFIG_DEFAULT();
    ec.sample_rate = 48000;              /* omni's contract; 16 kHz made the recogniser hallucinate */
    ec.channel = 1;
    ec.bits_per_sample = 16;
    ec.bitrate = 24000;
    ec.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    ec.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    ec.complexity = 2;                   /* low: CPU matters here, quality is tuned later */
    if (esp_opus_enc_open(&ec, sizeof(ec), &s_enc) != ESP_AUDIO_ERR_OK) { ESP_LOGE(TAG, "opus encoder"); return false; }
    int in_size = 0;
    esp_opus_enc_get_frame_size(s_enc, &in_size, &s_enc_out_len);
    s_enc_out = heap_caps_malloc(s_enc_out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "opus encoder: %d B in, up to %d B out per 20 ms", in_size, s_enc_out_len);

    esp_opus_dec_cfg_t dc = {
        .sample_rate = AUDIO_SAMPLE_RATE,   /* decode straight to the amp's rate */
        .channel = 1,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS,
        .self_delimited = false,
    };
    if (esp_opus_dec_open(&dc, sizeof(dc), &s_dec) != ESP_AUDIO_ERR_OK) { ESP_LOGE(TAG, "opus decoder"); return false; }

    s_down = xQueueCreateWithCaps(JB_DEPTH, sizeof(pkt_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    /* The token alone is over a kilobyte, and it rides in the query string because a
     * WebSocket handshake carries no Authorization header the gateway will read. */
    size_t ulen = strlen(CONFIG_BUDDY_VOICE_URL) + strlen(serial) + strlen(CONFIG_BUDDY_VOICE_TOKEN) + 32;
    char *url = heap_caps_malloc(ulen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    snprintf(url, ulen, "%s?serial=%s&token=%s", CONFIG_BUDDY_VOICE_URL, serial, CONFIG_BUDDY_VOICE_TOKEN);

    esp_websocket_client_config_t wc = {
        .uri = url,
        /* The handshake's request line carries that token, so the default 1 KB
         * buffer cannot even hold the request this client sends. */
        .buffer_size = 4096,
        .task_stack = 8192,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
    };
    if (!strncmp(CONFIG_BUDDY_VOICE_URL, "wss://", 6)) wc.cert_pem = isrg_root_x1_pem_start;
    s_ws = esp_websocket_client_init(&wc);
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws, NULL);

    /* Core 1 for audio, leaving core 0 to the panel and the radio, so a redraw can
     * never stall a conversation. Stacks in PSRAM: Opus encodes on the stack, and
     * internal RAM is what the TLS sessions and DMA are short of. */
    xTaskCreatePinnedToCoreWithCaps(up_task, "voice_up", 32768, NULL, 5, NULL, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    xTaskCreatePinnedToCoreWithCaps(down_task, "voice_down", 16384, NULL, 6, NULL, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "connecting to %s as %s", CONFIG_BUDDY_VOICE_URL, serial);
    return esp_websocket_client_start(s_ws) == ESP_OK;
}

#else   /* voice off: the rest of the firmware asks, and is told no */

bool voice_start(const char *serial) { (void)serial; return false; }
bool voice_active(void) { return false; }

#endif
