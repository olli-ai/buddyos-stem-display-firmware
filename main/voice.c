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
#if CONFIG_BUDDY_WAKE_WORD
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#endif
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
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
/* How long to wait for a late packet before calling it an underrun: less than the
 * 120 ms the I2S DMA holds (audio.c), so a packet that is merely late never becomes
 * silence. Measured before this existed: every momentarily empty queue counted as an
 * underrun and waited out a 100 ms refill against 90 ms of DMA — 54-59 audible gaps
 * over a few replies, most of them from packets a few milliseconds late. */
#define JB_WAIT_MS   80
/* "Waited-for" in the log is not a fault count: once playback keeps pace with omni the
 * queue sits near empty and most packets are waited for. Underruns are the audible
 * gaps — 8 over a four-reply conversation after this change, from 54-59 before. */
/* omni's silence frames are a few bytes; speech is tens. Only used to choose what to
 * shed first, so misjudging one costs 20 ms, not a word. */
#define SILENCE_MAX   8

typedef struct { uint16_t len; uint8_t data[PKT_MAX]; } pkt_t;

/* The uplink runs on two cores. The mic task (core 1) only captures, cleans the
 * signal and listens for the wake word; it hands 16 kHz PCM to the send task (core 0),
 * which upsamples, encodes and sends.
 *
 * Measured with all of it in the mic task: Opus 17.5 ms, WakeNet 4-13 ms, the x3
 * filter 2.5 ms per 20 ms frame — with the reply's decode (7.3 ms) and lip-sync on
 * the same core. Core 1 was at twice its capacity, the task watchdog fired, 500
 * frames took 17.5 s of wall time, and the I2S driver overwrote a third of the
 * child's voice before it was read. WakeNet, starved the same way, missed the wake
 * word while the Buddy talked. Sending was also blocking: send_bin waits on the
 * client lock while the WebSocket task receives omni's 50 packets a second. */
#define UP_PKT_MAX   160                 /* the encoder's own ceiling at 24 kbps */
#define UPQ_DEPTH    50                  /* 1 s: a network or encoder stall costs latency, not audio */
typedef struct { uint8_t len; uint16_t gen; uint32_t frame; uint8_t data[UP_PKT_MAX]; } up_pkt_t;
typedef struct { uint16_t gen; uint32_t frame; int16_t pcm[IN_SAMPLES]; } pcm_frame_t;
static QueueHandle_t s_pcmq;
static volatile uint32_t s_sent, s_up_dropped;

#if CONFIG_BUDDY_WAKE_WORD
/* A conversation, from the kit's side.
 *
 *   IDLE     the mic feeds WakeNet and nothing else; no socket, no audio leaves
 *   OPENING  heard the wake word; the socket is connecting, speech is held back
 *   OPEN     streaming; ends after CONFIG_BUDDY_VOICE_IDLE_S of quiet, or if it drops
 *
 * Owned by up_task alone — it is the only task that starts or stops the socket, since
 * the WebSocket client may not be stopped from its own event handler. */
typedef enum { PHASE_IDLE, PHASE_OPENING, PHASE_OPEN } phase_t;
static phase_t s_phase = PHASE_IDLE;

/* Speech spoken while the socket connects (TLS, then omni resolving the persona:
 * about a second and a half) is kept and sent the moment it opens, so a child who
 * says "Jarvis, what is a volcano" in one breath is heard in full. */
#define HOLD_PKTS    150                 /* 3 s */
static up_pkt_t *s_hold;                 /* owned by send_task */
static int s_hold_n;
/* Bumped by every new conversation, and carried by each packet, so the send task can
 * tell a held packet from a conversation that never opened from one that is current. */
static volatile uint16_t s_gen;
static volatile bool s_want_interrupt;   /* barge-in, sent by send_task */

#define OPEN_TIMEOUT_MS  12000           /* a socket that has not opened by now will not */
/* Above this a mic frame counts as the child talking, for the idle timer only. The
 * INMP441's floor measured -40 to -49 dBFS in a quiet room; speech at arm's length
 * sits near -25. */
#define VOICE_RMS    580                 /* -35 dBFS */

static volatile uint32_t s_last_activity_ms;

/* After a barge-in, the reply omni already sent is still arriving. It is dropped
 * until omni confirms it has stopped — any state change — or this long passes. */
#define BARGE_MAX_MS 1500
static volatile bool s_barged;
static uint32_t s_barged_ms;
static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
/* Milliseconds from `then` to `now`, signed. s_last_activity_ms is also written by the
 * WebSocket task, so it can be a hair *newer* than the up task's `now` — an unsigned
 * difference then wraps to 49 days and ends the conversation as "quiet" on the spot,
 * which is exactly what the first barge-in test did the moment omni said "processing". */
static inline int32_t since(uint32_t now, uint32_t then) { return (int32_t)(now - then); }

static const esp_wn_iface_t *s_wn;
static model_iface_data_t *s_wn_data;
static int16_t *s_wn_buf;
static int s_wn_chunk, s_wn_fill;
#endif

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
static volatile uint32_t s_shed_silence, s_shed_old, s_underruns, s_late;

bool voice_active(void) { return s_connected; }

static void show_state(const char *name)
{
    for (int i = 0; i < face_state_count; i++)
        if (!strcmp(face_states[i].name, name)) { face_view_request_state(i); return; }
}

/* omni's vocabulary, onto the face's. */
static void on_status(const char *st)
{
#if CONFIG_BUDDY_WAKE_WORD
    /* omni has acted on the interrupt (or moved on): its audio is current again. A
     * late "speaking_started" from before the interrupt is the one status that says
     * nothing new, so it does not end the window. */
    if (s_barged && strcmp(st, "speaking_started")) s_barged = false;
    else if (s_barged) { ESP_LOGI(TAG, "omni: %s (before the interrupt; ignored)", st); return; }
#endif
    if (!strcmp(st, "listening"))             { s_speaking = false; show_state("listening"); }
    else if (!strcmp(st, "processing"))       { show_state("thinking"); }
    else if (!strcmp(st, "speaking_started")) { s_speaking = true;  show_state("speaking"); }
    else if (!strcmp(st, "speaking_ended") || !strcmp(st, "interrupted"))
                                              { s_speaking = false; show_state("listening"); }
    else if (!strcmp(st, "error"))            { s_speaking = false; show_state("error"); }
#if CONFIG_BUDDY_WAKE_WORD
    s_last_activity_ms = now_ms();
#endif
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
#if CONFIG_BUDDY_WAKE_WORD
            if (s_barged) break;         /* the tail of an interrupted reply */
#endif
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

#if CONFIG_BUDDY_WAKE_WORD
static uint32_t s_opened_ms;

static uint32_t frame_rms(const int16_t *x, int n)
{
    uint64_t acc = 0;
    for (int i = 0; i < n; i++) acc += (int32_t)x[i] * x[i];
    uint32_t m = (uint32_t)(acc / n), r = 0;
    for (uint32_t b = 1u << 30; b; b >>= 2)          /* integer square root */
        if (r + b <= m) { m -= r + b; r = (r >> 1) + b; } else r >>= 1;
    return r;
}

/* 20 ms of mic in; true once WakeNet has heard the word. WakeNet wants its own chunk
 * size (512 samples for WakeNet9), so frames are gathered until one is full. */
static bool wake_feed(const int16_t *x, int n)
{
    static uint32_t chunks, us_total;
    bool heard = false;
    while (n) {
        int take = s_wn_chunk - s_wn_fill < n ? s_wn_chunk - s_wn_fill : n;
        memcpy(s_wn_buf + s_wn_fill, x, take * sizeof(int16_t));
        s_wn_fill += take; x += take; n -= take;
        if (s_wn_fill < s_wn_chunk) break;
        s_wn_fill = 0;
        int64_t t0 = esp_timer_get_time();
        if (s_wn->detect(s_wn_data, s_wn_buf) == WAKENET_DETECTED) heard = true;
        us_total += (uint32_t)(esp_timer_get_time() - t0);
        /* What listening costs, since it runs whenever the kit is on. */
        if (++chunks % 1000 == 0) {
            ESP_LOGI(TAG, "wakenet: %lu us per %d ms chunk", (unsigned long)(us_total / 1000),
                     s_wn_chunk * 1000 / AUDIO_SAMPLE_RATE);
            us_total = 0;
        }
    }
    return heard;
}

static void session_end(const char *why);

/* The wake word, heard mid-conversation. While the Buddy talks it means "stop, I
 * want to say something": omni is told to stop, what is queued here is thrown
 * away, and the mic is live again at once rather than when omni's status arrives.
 * At any other time it only counts as the child being there. */
static void barge_in(uint32_t now)
{
    s_last_activity_ms = now;
    if (!s_speaking || !s_connected) return;
    ESP_LOGI(TAG, "wake word while speaking: interrupting");
    s_want_interrupt = true;             /* the send task says it; this one keeps listening */
    s_barged = true;
    s_barged_ms = now;
    s_speaking = false;
    xQueueReset(s_down);
    show_state("listening");
}

static void session_begin(uint32_t now)
{
    ESP_LOGI(TAG, "wake word: opening a conversation");
    s_phase = PHASE_OPENING;
    s_opened_ms = s_last_activity_ms = now;
    s_gen++;
    /* Modem sleep makes the access point hold downlink traffic until each DTIM
     * beacon, so omni's steady 20 ms packets arrive in 100-300 ms clumps. Off for
     * the length of a conversation, back on when it ends. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    show_state("listening");
    if (esp_websocket_client_start(s_ws) != ESP_OK) session_end("could not start");
}

static void session_end(const char *why)
{
    ESP_LOGI(TAG, "conversation over (%s)", why);
    /* A clean close when there is someone to say goodbye to; otherwise just stop.
     * Either blocks this task for a moment, which costs a few mic frames nobody is
     * listening to. */
    if (s_connected) esp_websocket_client_close(s_ws, pdMS_TO_TICKS(2000));
    else esp_websocket_client_stop(s_ws);
    s_connected = false;
    s_speaking = false;
    xQueueReset(s_down);
    audio_speaker_enable(false);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    show_state("idle");
    /* Not s_wn->clean(): in ESP-SR 2.1.2 it faults inside WakeNet9's quantised model
     * (dl_convq_queue_bzero on a NULL queue), found when the first conversation
     * ended. WakeNet runs throughout a conversation now, so there is nothing stale
     * to forget anyway. */
    s_phase = PHASE_IDLE;
}

static bool wake_init(void)
{
    srmodel_list_t *models = esp_srmodel_init("model");
    char *name = models ? esp_srmodel_filter(models, ESP_WN_PREFIX, NULL) : NULL;
    if (!name) {
        ESP_LOGE(TAG, "no wake-word model in the `model` partition (flash srmodels.bin)");
        return false;
    }
    s_wn = esp_wn_handle_from_name(name);
    s_wn_data = s_wn->create(name, CONFIG_BUDDY_WAKE_THRESHOLD_MODE ? DET_MODE_95 : DET_MODE_90);
    if (!s_wn_data) { ESP_LOGE(TAG, "wakenet %s would not load", name); return false; }
    s_wn_chunk = s_wn->get_samp_chunksize(s_wn_data);
    s_wn_buf = heap_caps_malloc(s_wn_chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_hold = heap_caps_malloc(HOLD_PKTS * sizeof(up_pkt_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "wake word \"%s\" (%s), %d-sample chunks at %d Hz", s_wn->get_word_name(s_wn_data, 1),
             name, s_wn_chunk, s_wn->get_samp_rate(s_wn_data));
    return s_wn_buf && s_hold;
}
#endif

/* Mic to omni. */
static void up_task(void *arg)
{
    (void)arg;
    static pcm_frame_t f;
    float dc_x = 0, dc_y = 0;
    uint32_t frames = 0;
    int64_t t_work = 0;
    uint32_t work_max = 0;               /* worst 20 ms frame's processing, us */
    uint64_t us_wake = 0;

    for (;;) {
        if (t_work) {
            uint32_t w = (uint32_t)(esp_timer_get_time() - t_work);
            if (w > work_max) work_max = w;
        }
        int16_t *in = f.pcm;
        size_t n = audio_read(in, IN_SAMPLES);
        t_work = esp_timer_get_time();
        if (n < IN_SAMPLES) memset(in + n, 0, (IN_SAMPLES - n) * sizeof(int16_t));

        /* The INMP441's DC offset is large and wanders while its high-pass settles;
         * a one-pole blocker takes it out before it reaches the encoder. */
        for (int i = 0; i < IN_SAMPLES; i++) {
            float x = in[i], y = x - dc_x + 0.995f * dc_y;
            dc_x = x; dc_y = y;
            in[i] = clamp16((int32_t)y);
        }

#if CONFIG_BUDDY_WAKE_WORD
        /* WakeNet hears the room for as long as the kit is on — the real microphone,
         * before the half-duplex mute below — so the wake word both opens a
         * conversation and cuts into the Buddy mid-sentence. */
        uint32_t now = now_ms();
        int64_t t0 = esp_timer_get_time();
        bool heard = wake_feed(in, IN_SAMPLES);
        us_wake += esp_timer_get_time() - t0;
        if (s_phase == PHASE_IDLE) {
            if (heard) session_begin(now);
            continue;
        }
        if (heard) barge_in(now);
        if (s_barged && since(now, s_barged_ms) > BARGE_MAX_MS) s_barged = false;
        if (s_speaking || frame_rms(in, IN_SAMPLES) > VOICE_RMS) s_last_activity_ms = now;
        if (s_phase == PHASE_OPENING && s_connected) s_phase = PHASE_OPEN;
        const char *why = NULL;
        if (s_phase == PHASE_OPENING && since(now, s_opened_ms) > OPEN_TIMEOUT_MS) why = "never opened";
        else if (s_phase == PHASE_OPEN && !s_connected) why = "dropped";
        else if (since(now, s_last_activity_ms) > CONFIG_BUDDY_VOICE_IDLE_S * 1000) why = "quiet";
        if (why) { session_end(why); continue; }
        f.gen = s_gen;
#endif

        /* Half duplex, for now. While the Buddy talks, the microphone hears the
         * speaker, and omni would take its own voice for the child's and interrupt
         * itself. Silence keeps the stream continuous — omni wants it gapless — and
         * echo cancellation is what replaces this, later. Saying the wake word is how
         * a child gets a word in meanwhile. */
        if (s_speaking) memset(in, 0, IN_SAMPLES * sizeof(int16_t));

        f.frame = ++frames;
        if (xQueueSend(s_pcmq, &f, 0) != pdTRUE) s_up_dropped++;      /* never wait here */

        if (frames % 500 == 0) {
            ESP_LOGI(TAG, "mic: %lu frames, %lu sent, %lu dropped at the queue, worst frame %lu us "
                     "of 20000, wakenet %llu us/frame; stack unused %u B", (unsigned long)frames,
                     (unsigned long)s_sent, (unsigned long)s_up_dropped, (unsigned long)work_max,
                     us_wake / 500, (unsigned)uxTaskGetStackHighWaterMark(NULL));
            work_max = 0;
            us_wake = 0;
        }
    }
}

/* Encoded mic packets to the socket. Allowed to block — that is its whole job. */
static void send_task(void *arg)
{
    (void)arg;
    static pcm_frame_t f;
    static int16_t up[UP_SAMPLES];
    static up_pkt_t p;
    int16_t hist[VOICE_UP_PHASE] = { 0 };
    uint64_t us_fir = 0, us_enc = 0;
    uint32_t n = 0;
    for (;;) {
        xQueueReceive(s_pcmq, &f, portMAX_DELAY);

        /* x3, polyphase: newest sample first, three outputs per input. The filter is
         * generated by tools/design_upsampler.py and was validated against omni's
         * recogniser before a line of this was written. */
        int64_t t0 = esp_timer_get_time();
        for (int i = 0; i < IN_SAMPLES; i++) {
            memmove(hist + 1, hist, (VOICE_UP_PHASE - 1) * sizeof(int16_t));
            hist[0] = f.pcm[i];
            for (int ph = 0; ph < VOICE_UP_L; ph++) {
                int32_t acc = 0;
                for (int k = 0; k < VOICE_UP_PHASE; k++) acc += voice_up_phase[ph][k] * hist[k];
                up[i * VOICE_UP_L + ph] = clamp16((acc + (1 << 14)) >> 15);
            }
        }
        int64_t t1 = esp_timer_get_time();
        esp_audio_enc_in_frame_t fin = { .buffer = (uint8_t *)up, .len = sizeof(up) };
        esp_audio_enc_out_frame_t fout = { .buffer = s_enc_out, .len = (uint32_t)s_enc_out_len };
        esp_audio_err_t er = esp_opus_enc_process(s_enc, &fin, &fout);
        us_fir += t1 - t0;
        us_enc += esp_timer_get_time() - t1;
        if (++n % 500 == 0) {
            ESP_LOGI(TAG, "send: per 20 ms frame: x3 FIR %llu us, opus %llu us; stack unused %u B",
                     us_fir / 500, us_enc / 500, (unsigned)uxTaskGetStackHighWaterMark(NULL));
            us_fir = us_enc = 0;
        }
        if (er != ESP_AUDIO_ERR_OK || !fout.encoded_bytes || fout.encoded_bytes > UP_PKT_MAX) continue;
        p.len = (uint8_t)fout.encoded_bytes;
        p.gen = f.gen;
        p.frame = f.frame;
        memcpy(p.data, s_enc_out, fout.encoded_bytes);

#if CONFIG_BUDDY_WAKE_WORD
        if (s_want_interrupt && s_connected) {
            static const char msg[] = "{\"type\":\"interrupt\"}";
            esp_websocket_client_send_text(s_ws, msg, sizeof(msg) - 1, pdMS_TO_TICKS(200));
            s_want_interrupt = false;
        }
        if (s_phase == PHASE_IDLE) { s_hold_n = 0; continue; }
        if (s_hold_n && s_hold[0].gen != p.gen) s_hold_n = 0;   /* from a chat that never opened */
        if (!s_connected) {
            if (s_hold_n < HOLD_PKTS) s_hold[s_hold_n++] = p;
            continue;
        }
        if (s_hold_n) {
            int flushed = 0;
            for (int i = 0; i < s_hold_n; i++)
                if (esp_websocket_client_send_bin(s_ws, (const char *)s_hold[i].data, s_hold[i].len,
                                                  pdMS_TO_TICKS(200)) > 0) flushed++;
            ESP_LOGI(TAG, "sent %d of %d packets held while connecting", flushed, s_hold_n);
            s_hold_n = 0;
        }
#endif
        if (!s_connected) continue;
        if (esp_websocket_client_send_bin(s_ws, (const char *)p.data, p.len, pdMS_TO_TICKS(200)) > 0)
            s_sent++;
#if CONFIG_BUDDY_VOICE_TAP
        /* The exact bytes omni was sent, while it is the child's turn. Printed here,
         * not in the mic task: a UART line costs ~8 ms, which the mic cannot spare. */
        if (!s_speaking) {
            static unsigned char b64[256];
            size_t olen = 0;
            if (mbedtls_base64_encode(b64, sizeof(b64), &olen, p.data, p.len) == 0)
                printf("TAP %lu %.*s\n", (unsigned long)p.frame, (int)olen, b64);
        }
#endif
    }
}

/* omni to the amp, and to the mouth. */
static void down_task(void *arg)
{
    (void)arg;
    static pkt_t p;
    static int16_t pcm[960];             /* up to 60 ms at 16 kHz */
    bool was_speaking = false;

    uint64_t us_dec = 0, us_lip = 0;     /* per-stage totals, for the log */
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
            if (xQueueReceive(s_down, &p, pdMS_TO_TICKS(JB_WAIT_MS)) != pdTRUE) {
                s_underruns++;
                refill = true;           /* I2S plays silence meanwhile: auto_clear */
                continue;
            }
            s_late++;                    /* waited for it; the DMA covered the wait */
        }
        if (++played % 500 == 0) {       /* every 10 s of playback */
            ESP_LOGI(TAG, "down: held %u, shed %lu silence + %lu old, %lu waited-for, %lu underruns",
                     (unsigned)uxQueueMessagesWaiting(s_down), (unsigned long)s_shed_silence,
                     (unsigned long)s_shed_old, (unsigned long)s_late, (unsigned long)s_underruns);
            ESP_LOGI(TAG, "down: per 20 ms packet: opus decode %llu us, lip-sync %llu us",
                     us_dec / 500, us_lip / 500);
            us_dec = us_lip = 0;
        }
        esp_audio_dec_in_raw_t raw = { .buffer = p.data, .len = p.len };
        esp_audio_dec_out_frame_t fo = { .buffer = (uint8_t *)pcm, .len = sizeof(pcm) };
        esp_audio_dec_info_t info;
        int64_t t0 = esp_timer_get_time();
        if (esp_opus_dec_decode(s_dec, &raw, &fo, &info) != ESP_AUDIO_ERR_OK) continue;
        us_dec += esp_timer_get_time() - t0;
        size_t ns = fo.decoded_size / sizeof(int16_t);
        audio_write(pcm, ns);

        /* The same samples that go to the speaker drive the mouth, so the lips follow
         * what the child actually hears. Lip-sync state lives on this task alone. */
        bool sp = s_speaking;
        if (sp && !was_speaking) face_lipsync_start();
        if (!sp && was_speaking) { face_lipsync_stop(); face_view_set_live_mouth(-1); }
        if (sp) {
            t0 = esp_timer_get_time();
            face_view_set_live_mouth(face_lipsync_feed(pcm, ns));
            us_lip += esp_timer_get_time() - t0;
        }
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

#if CONFIG_BUDDY_WAKE_WORD
    if (!wake_init()) return false;
#endif

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
    s_pcmq = xQueueCreateWithCaps(UPQ_DEPTH, sizeof(pcm_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

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
#if CONFIG_BUDDY_WAKE_WORD
        /* A dropped conversation ends; the child says the word again. Reconnecting on
         * its own would open a session nobody asked for. */
        .disable_auto_reconnect = true,
#endif
    };
    if (!strncmp(CONFIG_BUDDY_VOICE_URL, "wss://", 6)) wc.cert_pem = isrg_root_x1_pem_start;
    s_ws = esp_websocket_client_init(&wc);
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws, NULL);

    /* Core 1 for the mic, WakeNet and playback, which must never wait; core 0 for
     * encoding and sending, alongside the panel and the radio (see s_pcmq). Stacks in
     * PSRAM: internal RAM is what the TLS sessions and DMA are short of, and an
     * internal stack measured no faster for Opus. */
    xTaskCreatePinnedToCoreWithCaps(up_task, "voice_up", 16384, NULL, 5, NULL, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    xTaskCreatePinnedToCoreWithCaps(down_task, "voice_down", 16384, NULL, 6, NULL, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* Opus encodes on this stack: ~21 KB measured. */
    xTaskCreatePinnedToCoreWithCaps(send_task, "voice_send", 28672, NULL, 5, NULL, 0,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

#if CONFIG_BUDDY_WAKE_WORD
    /* A kit waiting to be spoken to looks idle, not like the demo reel: the demo's
     * cycle runs through "error" and "speaking", which say something untrue here. */
    show_state("idle");
    ESP_LOGI(TAG, "listening for the wake word; will talk to %s as %s", CONFIG_BUDDY_VOICE_URL, serial);
    return true;
#else
    esp_wifi_set_ps(WIFI_PS_NONE);       /* always in a conversation: see session_begin */
    ESP_LOGI(TAG, "connecting to %s as %s", CONFIG_BUDDY_VOICE_URL, serial);
    return esp_websocket_client_start(s_ws) == ESP_OK;
#endif
}

#else   /* voice off: the rest of the firmware asks, and is told no */

bool voice_start(const char *serial) { (void)serial; return false; }
bool voice_active(void) { return false; }

#endif
