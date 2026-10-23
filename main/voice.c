#include "voice.h"
#include "sleep.h"
#include "sdkconfig.h"

#if CONFIG_BUDDY_VOICE
#include "audio.h"
#if CONFIG_BUDDY_SCREEN_FACE
#include "face_lipsync.h"
#include "face_directive.h"
#include "face_view.h"
#else
#include "lipsync.h"
#endif
#include "status.h"
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
#if CONFIG_BUDDY_AEC
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#endif
#include "cJSON.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "voice";

/* The mouth, whichever screen owns it. The face classifier answers with a sprite to
 * draw and the view takes it — only the mouth, so the eyes and brows go on blinking
 * and tilting through a reply. The gallery's answers with a viseme class, which a
 * photograph can only approximate by dropping its jaw, and reads it back itself. */
#if CONFIG_BUDDY_SCREEN_FACE
static inline void mouth_start(void) { face_lipsync_start(); }
static inline void mouth_stop(void) { face_lipsync_stop(); face_view_set_live_mouth(-1); }
static inline void mouth_feed(const int16_t *pcm, size_t n) { face_view_set_live_mouth(face_lipsync_feed(pcm, n)); }
#else
static inline void mouth_start(void) { }
static inline void mouth_stop(void) { lipsync_stop(); }
static inline void mouth_feed(const int16_t *pcm, size_t n) { lipsync_feed(pcm, n); }
#endif

/* omni's vocabulary onto the face's. The face is the state display: a child reads a
 * Buddy that is plainly listening, or plainly thinking, long before they read a word
 * in a pill — and this is why the face has states at all. The pill stays for the
 * words, and for the gallery build, which has no face to put them on.
 *
 * face_view_request_state() only records; the render tick applies it. It also marks
 * the face driven, which stops the demo cycle for good — right, because from the
 * first conversation onwards something real has an opinion about the face. */
#if CONFIG_BUDDY_SCREEN_FACE
/* States that begin a turn, and so end whatever expression the last one was wearing.
 * `thinking` and `speaking` are the turn itself and must not clear it: the expression
 * is chosen during the first and belongs to the second. */
static bool starts_a_turn(const char *name)
{
    return !strcmp(name, "listening") || !strcmp(name, "idle")
        || !strcmp(name, "busy")      || !strcmp(name, "error");
}

#if CONFIG_BUDDY_SCREEN_FACE
static bool s_face_resting;              /* the face is wearing `asleep`, not `idle` */
/* Asked for, rather than arrived at. The quiet timer below rests the face when the
 * room goes silent and wakes it on any sound — which would undo a "bye bye" within a
 * second, because saying goodbye is a sound. So a requested rest holds until the wake
 * word, and the timer stands aside while it does. */
static volatile bool s_face_rest_held;
#endif

static void show_state(const char *name)
{
    const char *want = name;
#if CONFIG_BUDDY_SCREEN_FACE && CONFIG_BUDDY_FACE_ASLEEP_S > 0
    /* A skill asked the face to settle, and the conversation has not closed yet.
     *
     * The socket stays open for CONFIG_BUDDY_VOICE_IDLE_S after the last word, and
     * the quiet timer that honours a held rest only runs once it has — so a Buddy
     * told "bye bye" went on wearing `listening` for another fifteen seconds, which
     * is exactly as long as it takes to look like nothing happened.
     *
     * So the rest lands at the first lull instead: the moment the Buddy stops
     * speaking. Not during `speaking` — the goodbye should be said with a face on —
     * and not during `thinking`, which is still part of answering. */
    if (s_face_rest_held && (!strcmp(name, "listening") || !strcmp(name, "idle"))) {
        want = "asleep";
        s_face_resting = true;
        /* And the pill with it. on_status sets STATUS_LISTENING before calling here,
         * so without this a settled Buddy sits under a "Listening" badge — the face
         * says goodnight and the label argues with it. STATUS_IDLE is the waiting-
         * for-the-wake-word one, which is exactly what the kit is now doing. */
        status_set(STATUS_IDLE);
    }
#endif
    for (int i = 0; i < face_state_count; i++)
        if (!strcmp(face_states[i].name, want)) {
            /* On the state that was asked for, not the one substituted above. */
            if (starts_a_turn(name)) face_view_request_expression(NULL);
            face_view_request_state(i);
            return;
        }
}
#else
static inline void show_state(const char *name) { (void)name; }
#endif

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

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
/* Milliseconds from `then` to `now`, signed. s_last_activity_ms is also written by the
 * WebSocket task, so it can be a hair *newer* than the up task's `now` — an unsigned
 * difference then wraps to 49 days and ends the conversation as "quiet" on the spot,
 * which is exactly what the first barge-in test did the moment omni said "processing". */
static inline int32_t since(uint32_t now, uint32_t then) { return (int32_t)(now - then); }

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

#if !CONFIG_BUDDY_AEC
static const esp_wn_iface_t *s_wn;
static model_iface_data_t *s_wn_data;
static int16_t *s_wn_buf;
static int s_wn_chunk, s_wn_fill;
#endif
#endif

static esp_websocket_client_handle_t s_ws;
static QueueHandle_t s_down;             /* omni's packets, waiting to be spoken */
static void *s_enc, *s_dec;
static uint8_t *s_enc_out;
static int s_enc_out_len;
static volatile bool s_connected;
/* While the Buddy is talking. Written by the WebSocket task, read by the audio tasks. */
static volatile bool s_speaking;
static volatile uint32_t s_shed_silence, s_shed_old, s_underruns, s_late;

#if CONFIG_BUDDY_SCREEN_FACE
/* Expressions, held until the audio they belong to is actually being heard.
 *
 * omni already places each [FACE:x] by *audio* rather than by text: tts.py splits
 * the stream at the tag and reports `at_ms`, the tag's position in this turn's
 * audio. But it fires when omni *sends* that audio, and the kit plays it later —
 * after the jitter buffer, the decoder and the I2S DMA. Applying on arrival
 * therefore ran the face ahead of the voice, and a reply with several tags showed
 * them in a burst while the kit was still speaking the first sentence.
 *
 * So the kit keeps its own clock: milliseconds of this turn's audio actually fed to
 * the speaker, zeroed when a turn's speech starts. An expression waits in this queue
 * until that clock reaches its `at_ms`. */
#define EXPR_PENDING 8
typedef struct { uint32_t at_ms; char id[20]; } expr_due_t;
static QueueHandle_t s_expr_q;
static volatile uint32_t s_play_ms;
#endif

/* omni's own barge-in, which needs no wake word. When its voice activity detection
 * hears the child over the reply it stops mid-sentence and says so: "flush_audio"
 * first, then "interrupted" a millisecond later.
 *
 * Only the kit can act on the first of those. omni can stop sending, but what it
 * already sent is here — up to JB_DEPTH packets queued and another 120 ms inside the
 * codec's DMA — so without this the Buddy talks over the child for most of a second
 * after omni has fallen silent, which reads as not having been heard at all. The
 * queue is emptied on flush_audio, and packets that were behind it in the socket are
 * dropped for this long: omni cancels its own TTS task *after* sending flush_audio,
 * so a few more packets of the abandoned reply can still be on their way.
 *
 * Counted, because a flush that never arrives and a flush that arrives and does
 * nothing sound exactly alike from the other side of the room. Each flush also costs
 * one underrun in the same log line — the queue it empties is the queue down_task
 * then finds empty — so read the two together. */
#define FLUSH_DROP_MS 300
static volatile bool s_flushing;
static volatile uint32_t s_flushed_ms;
static volatile uint32_t s_flushes, s_flush_dropped;

bool voice_active(void) { return s_connected; }

/* omni's turn-taking, as it arrives. */
static void on_status(const char *st)
{
#if CONFIG_BUDDY_WAKE_WORD
    /* omni has acted on the interrupt (or moved on): its audio is current again. A
     * late "speaking_started" from before the interrupt is the one status that says
     * nothing new, so it does not end the window. */
    if (s_barged && strcmp(st, "speaking_started")) s_barged = false;
    else if (s_barged) { ESP_LOGI(TAG, "omni: %s (before the interrupt; ignored)", st); return; }
#endif
    /* Everything in hand is the sentence omni abandoned: drop it rather than play it
     * over the child. The face and the status are left to the "interrupted" right
     * behind this, which is the status that means the turn is theirs. */
    if (!strcmp(st, "flush_audio")) {
        s_flushing = true;
        s_flushed_ms = now_ms();
        s_flushes++;
        s_speaking = false;
        xQueueReset(s_down);
        mouth_stop();                    /* the mouth shuts with the speaker */
#if CONFIG_BUDDY_SCREEN_FACE
        /* Barge-in: the rest of that reply is never spoken, so the faces chosen for
         * it must not appear over whatever is said next. */
        if (s_expr_q) xQueueReset(s_expr_q);
        s_play_ms = 0;
#endif
    } else if (!strcmp(st, "speaking_started")) {
        s_flushing = false;              /* a new reply: what arrives now is current */
#if CONFIG_BUDDY_SCREEN_FACE
        /* A turn's audio starts here, so the clock its expressions are timed against
         * does too. The queue is NOT cleared: a tag at the very start of a reply has
         * at_ms 0 and can arrive a moment before this status does. */
        s_play_ms = 0;
#endif
    }

    if (!strcmp(st, "speaking_started")) s_speaking = true;
    else if (!strcmp(st, "listening") || !strcmp(st, "speaking_ended") ||
             !strcmp(st, "interrupted") || !strcmp(st, "error")) s_speaking = false;
    if (!strcmp(st, "interrupted")) mouth_stop();
    if (!strcmp(st, "speaking_started")) { status_set(STATUS_SPEAKING); show_state("speaking"); }
    else if (!strcmp(st, "processing") || !strcmp(st, "thinking")) { status_set(STATUS_THINKING); show_state("thinking"); }
    else if (!strcmp(st, "error")) { status_set(STATUS_ERROR); show_state("error"); }
    else if (!strcmp(st, "listening") || !strcmp(st, "speaking_ended") ||
             !strcmp(st, "interrupted")) { status_set(STATUS_LISTENING); show_state("listening"); }
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
#if CONFIG_BUDDY_SCREEN_FACE
        /* Say what this face can wear, before anything is said to the child. omni's
         * visual_face_emotion plugin arms on this and nothing else: no advertisement,
         * no face vocabulary in the model's prompt, and the Buddy talks with the one
         * expression its design gives the state. Sent on every connect because a
         * reconnect is a new session to omni, which remembers nothing. */
        {
            char ad[384];   /* the whole vocabulary, with room to spare */
            size_t n = face_expressions_advertise(ad, sizeof ad);
            if (n && esp_websocket_client_send_text(s_ws, ad, n, pdMS_TO_TICKS(200)) > 0)
                ESP_LOGI(TAG, "offered this face's expressions");
            else
                ESP_LOGW(TAG, "could not offer expressions; the face will not follow speech");
        }
#endif
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        s_connected = false;
        s_speaking = false;
        s_flushing = false;
        audio_speaker_enable(false);
#if CONFIG_BUDDY_WAKE_WORD
        if (s_phase != PHASE_IDLE) status_set(STATUS_CONNECTING);
#else
        status_set(STATUS_CONNECTING);   /* it reconnects on its own */
#endif
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
            /* The same tail, when omni interrupted itself rather than being told to. */
            if (s_flushing) {
                if (since(now_ms(), s_flushed_ms) <= FLUSH_DROP_MS) { s_flush_dropped++; break; }
                s_flushing = false;
            }
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
#if CONFIG_BUDDY_SCREEN_FACE
            else if (cJSON_IsString(type) && !strcmp(type->valuestring, "face_emotion")) {
                /* The model chose a face for what it is about to say. `at_ms` is the
                 * offset into this turn's audio where it belongs; the kit wears it on
                 * arrival instead, which is early by however much audio is queued.
                 * Scheduling it against playback is the next thing, and it wants its
                 * own measurement — a face that changes late reads worse than one
                 * that changes a moment early. */
                const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(j, "emotion"));
                const cJSON *at = cJSON_GetObjectItem(j, "at_ms");
                if (!id || !*id) break;
                expr_due_t d = { .at_ms = cJSON_IsNumber(at) && at->valuedouble > 0
                                          ? (uint32_t)at->valuedouble : 0 };
                strlcpy(d.id, id, sizeof d.id);
                /* Full means the model is tagging faster than they can be worn; drop
                 * the oldest, which is the one most likely already overtaken. */
                if (s_expr_q && xQueueSend(s_expr_q, &d, 0) != pdTRUE) {
                    expr_due_t old;
                    xQueueReceive(s_expr_q, &old, 0);
                    xQueueSend(s_expr_q, &d, 0);
                }
            }
#endif
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

#if CONFIG_BUDDY_UPLINK_GAIN_DB > 0
/* The cloud hears a louder kit than the wake word does. See BUDDY_UPLINK_GAIN_DB:
 * this board reaches the front end around -47 dBFS where a browser arrives near
 * -20, and omni gates speech onsets on absolute loudness, so without this the kit
 * is audible to its recogniser and invisible to its interrupter.
 *
 * Applied here rather than to the microphone itself because WakeNet lives inside
 * the AFE: lifting the analogue gain changes what it hears too, and on this board
 * it also wrecked the echo reference. Q8, so a multiply and a shift per sample —
 * about 20 us per 20 ms frame on the P4, measured against the 677 us the x3 filter
 * beside it already costs. */
#define UP_GAIN_Q 8
static int32_t s_up_gain;            /* 10^(dB/20) in Q8, set once at start */
static inline void uplink_gain(int16_t *pcm, int n)
{
    for (int i = 0; i < n; i++)
        pcm[i] = clamp16((int32_t)pcm[i] * s_up_gain >> UP_GAIN_Q);
}
#else
static inline void uplink_gain(int16_t *pcm, int n) { (void)pcm; (void)n; }
#endif

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

#if !CONFIG_BUDDY_AEC
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
#endif

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
    mouth_stop();                        /* the mouth shuts with the speaker */
}

static void session_begin(uint32_t now)
{
    ESP_LOGI(TAG, "wake word: opening a conversation");
#if CONFIG_BUDDY_SCREEN_FACE
    s_face_resting = false;              /* awake again, whatever the face was doing */
    s_face_rest_held = false;            /* and a held rest ends when spoken to */
#endif
    status_set(STATUS_CONNECTING);
    show_state("busy");                  /* heard, and working on it */
    sleep_note_activity();      /* being talked to is the clearest activity there is */
    s_phase = PHASE_OPENING;
    s_opened_ms = s_last_activity_ms = now;
    s_gen++;
    /* Modem sleep makes the access point hold downlink traffic until each DTIM
     * beacon, so omni's steady 20 ms packets arrive in 100-300 ms clumps. Off for
     * the length of a conversation, back on when it ends. */
    esp_wifi_set_ps(WIFI_PS_NONE);
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
    s_flushing = false;
    xQueueReset(s_down);
    mouth_stop();                        /* the mouth shuts with the speaker */
    audio_speaker_enable(false);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    /* Not s_wn->clean(): in ESP-SR 2.1.2 it faults inside WakeNet9's quantised model
     * (dl_convq_queue_bzero on a NULL queue), found when the first conversation
     * ended. WakeNet runs throughout a conversation now, so there is nothing stale
     * to forget anyway. */
    s_phase = PHASE_IDLE;
    status_set(STATUS_IDLE);
    show_state("idle");
}

#if CONFIG_BUDDY_AEC
/* Echo cancellation (the ESP32-P4 board). The microphones no longer go straight to
 * WakeNet: every ES7210 slot goes into ESP-SR's audio front end, which takes the
 * speaker's echo out of each microphone (AEC, against the reference slot), separates
 * the two microphones into sources (BSS), runs WakeNet on each and hands back the one
 * that heard the word. A feed task keeps it supplied; the mic task fetches from it in
 * place of reading the microphone, re-cut into the 20 ms frames the rest of this file
 * is built on.
 *
 * The reference is the ES7210's loopback slot when the board has one ('R' in
 * BUDDY_AFE_INPUT_FORMAT). When it has not, the first unused slot carries a software
 * reference instead — what audio_write() played, delayed by the I2S queue. */
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static int16_t *s_afe_out;
static int s_afe_cap, s_afe_have, s_afe_pos;
static int s_soft_ref = -1;

/* The feed and fetch sides each log what they saw this often, idle or not: without it
 * a wake word that never fires looks the same as a kit that is not listening. */
#define AFE_STATS_MS 10000

static void afe_out_stats(const afe_fetch_result_t *r)
{
    static uint32_t fetches, t_log;
    static float vol_max = -200, free_min = 1;
    static int states[3];
    fetches++;
    if (r->data_volume > vol_max) vol_max = r->data_volume;
    if (r->ringbuff_free_pct < free_min) free_min = r->ringbuff_free_pct;
    if (r->wakeup_state >= -1 && r->wakeup_state <= 1) states[r->wakeup_state + 1]++;   /* verified -1, none 0, detected 1 */
    if (!t_log) t_log = now_ms();
    if (since(now_ms(), t_log) < AFE_STATS_MS) return;
    ESP_LOGI(TAG, "afe out: %lu fetches, loudest %.1f dB, ring free min %.0f%%, wakenet none/detected/verified %d/%d/%d",
             (unsigned long)fetches, vol_max, free_min * 100, states[1], states[2], states[0]);
    fetches = 0; vol_max = -200; free_min = 1;
    memset(states, 0, sizeof(states));
    t_log = now_ms();
}

static void feed_task(void *arg)
{
    (void)arg;
    int chunk = s_afe->get_feed_chunksize(s_afe_data);
    int16_t *slots = heap_caps_malloc(chunk * AUDIO_IN_SLOTS * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *ref = heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!slots || !ref) { ESP_LOGE(TAG, "afe feed: no memory"); vTaskDelete(NULL); }
    /* What reaches the front end, slot by slot: a microphone that reads ~0 here is a
     * codec or slot-order fault, and nothing downstream can hear the wake word. */
    double sq[AUDIO_IN_SLOTS] = { 0 };
    int peak[AUDIO_IN_SLOTS] = { 0 };
    uint32_t n = 0, feeds = 0, t_log = now_ms();
    for (;;) {
        if (audio_read_slots(slots, chunk) != (size_t)chunk) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (s_soft_ref >= 0) {
            audio_ref_read(ref, chunk);
            for (int i = 0; i < chunk; i++) slots[i * AUDIO_IN_SLOTS + s_soft_ref] = ref[i];
        }
        for (int i = 0; i < chunk; i++)
            for (int c = 0; c < AUDIO_IN_SLOTS; c++) {
                int v = slots[i * AUDIO_IN_SLOTS + c];
                sq[c] += (double)v * v;
                if (abs(v) > peak[c]) peak[c] = abs(v);
            }
        n += chunk;
        s_afe->feed(s_afe_data, slots);
        feeds++;
        if (since(now_ms(), t_log) >= AFE_STATS_MS) {
            char line[96];
            int at = 0;
            for (int c = 0; c < AUDIO_IN_SLOTS; c++)
                at += snprintf(line + at, sizeof(line) - at, " %c:%d/%d", CONFIG_BUDDY_AFE_INPUT_FORMAT[c],
                               (int)sqrt(sq[c] / n), peak[c]);
            ESP_LOGI(TAG, "afe in: %lu feeds, rms/peak%s", (unsigned long)feeds, line);
            memset(sq, 0, sizeof(sq));
            memset(peak, 0, sizeof(peak));
            n = feeds = 0;
            t_log = now_ms();
        }
    }
}

/* 20 ms of the cleaned-up microphone into `out`; true if WakeNet heard the word in it. */
static bool mic_frame(int16_t *out)
{
    bool heard = false;
    int k = 0;
    while (k < IN_SAMPLES) {
        if (s_afe_pos == s_afe_have) {
            afe_fetch_result_t *r = s_afe->fetch(s_afe_data);
            if (!r || r->ret_value == ESP_FAIL || !r->data || r->data_size <= 0) {
                memset(out + k, 0, (IN_SAMPLES - k) * sizeof(int16_t));
                vTaskDelay(pdMS_TO_TICKS(20));      /* never spin on a front end that is not there */
                return heard;
            }
            if (r->wakeup_state == WAKENET_DETECTED) {
                heard = true;
                ESP_LOGI(TAG, "wake word heard (word %d, channel %d, %.1f dB)",
                         r->wake_word_index, r->trigger_channel_id, r->data_volume);
            }
            afe_out_stats(r);
            int n = r->data_size / (int)sizeof(int16_t);
            if (n > s_afe_cap) n = s_afe_cap;
            memcpy(s_afe_out, r->data, n * sizeof(int16_t));
            s_afe_have = n;
            s_afe_pos = 0;
        }
        int take = IN_SAMPLES - k < s_afe_have - s_afe_pos ? IN_SAMPLES - k : s_afe_have - s_afe_pos;
        memcpy(out + k, s_afe_out + s_afe_pos, take * sizeof(int16_t));
        k += take;
        s_afe_pos += take;
    }
    return heard;
}

static bool wake_init(void)
{
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models || !esp_srmodel_filter(models, ESP_WN_PREFIX, NULL)) {
        ESP_LOGE(TAG, "no wake-word model in the `model` partition (flash srmodels.bin)");
        return false;
    }
    char fmt[AUDIO_IN_SLOTS + 1];
    strlcpy(fmt, CONFIG_BUDDY_AFE_INPUT_FORMAT, sizeof(fmt));
    if (!strchr(fmt, 'R')) {
        char *spare = strchr(fmt, 'N');
        if (!spare) {
            ESP_LOGE(TAG, "slot layout \"%s\" has no reference and no spare slot to carry one", fmt);
            return false;
        }
        *spare = 'R';
        s_soft_ref = (int)(spare - fmt);
    }
    afe_config_t *cfg = afe_config_init(fmt, models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!cfg) { ESP_LOGE(TAG, "afe: no config for \"%s\"", fmt); return false; }
    cfg->aec_init = true;
    /* Full duplex, not speech recognition. afe_config_init()'s AFE_TYPE_SR picks
     * AEC_MODE_SR_HIGH_PERF, which is tuned for one job: hear the wake word over the
     * speaker. It has no reason to leave audio clean enough for a listener at the far
     * end, and the board says so in its own log line — AEC(SR_HIGH_PERF, NLP_OFF).
     * What the linear filter leaves behind is the Buddy's own voice, and the cloud's
     * speech detector takes it for the child. The FD modes exist for exactly this,
     * and NLP is the stage that suppresses the residue.
     *
     * Two costs to measure on the board rather than assume. FD is the more expensive
     * filter, and it runs on core 1 beside the mic task, which has overrun before.
     * And NLP suppresses whoever is quieter during playback — the child as well as
     * the speaker — which is the signal barge-in depends on. If the speaker still
     * leaks, VERYAGGR; if the child stops being heard mid-reply, NORMAL. */
    cfg->aec_mode = AEC_MODE_FD_HIGH_PERF;
    /* Configurable, because the two failures either side of it are both real here
     * and which one bites depends on the enclosure, the volume and the room.
     *
     * Turned down it lets the kit hear its own speaker, and omni's speech detector
     * takes that for the child: the Buddy interrupts itself mid-sentence. Turned up
     * it suppresses whoever is quieter during playback, and on this board that is
     * everybody — the microphone delivers speech at about -47 dBFS where this file
     * reckons arm's length to be -25 — so WakeNet runs nearer its threshold and the
     * wake word was intermittent when this was last turned up. Whichever way it is
     * set, the real fix is level rather than suppression; see the mic gain note. */
    static const aec_nlp_level_t NLP[] = {
        AEC_NLP_LEVEL_NORMAL, AEC_NLP_LEVEL_AGGR, AEC_NLP_LEVEL_VERYAGGR,
    };
    cfg->aec_nlp_level = NLP[CONFIG_BUDDY_AEC_NLP_LEVEL];
    cfg->wakenet_init = true;
    cfg->wakenet_mode = CONFIG_BUDDY_WAKE_THRESHOLD_MODE ? DET_MODE_95 : DET_MODE_90;
    cfg->vad_init = false;               /* the session's own level gate decides "quiet" */
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_perferred_core = 1;         /* beside the mic task; the radio has core 0 */
    cfg->afe_perferred_priority = 5;
    cfg = afe_config_check(cfg);
    s_afe = esp_afe_handle_from_config(cfg);
    s_afe_data = s_afe ? s_afe->create_from_config(cfg) : NULL;
    if (!s_afe_data) { ESP_LOGE(TAG, "afe would not start"); afe_config_free(cfg); return false; }
    ESP_LOGI(TAG, "afe: slots \"%s\"%s, wake word model %s", fmt,
             s_soft_ref >= 0 ? " (software reference)" : "", cfg->wakenet_model_name ? cfg->wakenet_model_name : "?");
    afe_config_free(cfg);
    s_afe->print_pipeline(s_afe_data);

    s_afe_cap = s_afe->get_fetch_chunksize(s_afe_data);
    s_afe_out = heap_caps_malloc(s_afe_cap * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_hold = heap_caps_malloc(HOLD_PKTS * sizeof(up_pkt_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_afe_out || !s_hold) return false;
    ESP_LOGI(TAG, "afe: feed %d x %d, fetch %d samples at %d Hz", s_afe->get_feed_chunksize(s_afe_data),
             s_afe->get_feed_channel_num(s_afe_data), s_afe_cap, s_afe->get_samp_rate(s_afe_data));
    if (s_soft_ref >= 0) audio_ref_enable(true);
    return xTaskCreatePinnedToCore(feed_task, "afe_feed", 8192, NULL, 6, NULL, 0) == pdPASS;
}
#else
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
#endif   /* CONFIG_BUDDY_AEC */
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
#if CONFIG_BUDDY_AEC
        bool heard = mic_frame(in);      /* the front end ran WakeNet already */
        t_work = esp_timer_get_time();
#else
        size_t n = audio_read(in, IN_SAMPLES);
        t_work = esp_timer_get_time();
        if (n < IN_SAMPLES) memset(in + n, 0, (IN_SAMPLES - n) * sizeof(int16_t));
#endif

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
#if !CONFIG_BUDDY_AEC
        int64_t t0 = esp_timer_get_time();
        bool heard = wake_feed(in, IN_SAMPLES);
        us_wake += esp_timer_get_time() - t0;
#endif
        if (s_phase == PHASE_IDLE) {
            if (heard) { session_begin(now); continue; }
#if CONFIG_BUDDY_SCREEN_FACE && CONFIG_BUDDY_FACE_ASLEEP_S > 0
            /* Let the face rest when the room goes quiet. A kit between conversations
             * holds `idle` indefinitely, so an idle state a child gave a lively loop —
             * the website's shake is the obvious one — would otherwise run for hours
             * at nobody. `asleep` is what the designer means by a Buddy at rest.
             *
             * Its own quiet check, because the session's one is only reached while a
             * conversation is open: the `continue` above is in the way. */
            if (frame_rms(in, IN_SAMPLES) > VOICE_RMS) s_last_activity_ms = now;
            bool rest = s_face_rest_held
                     || since(now, s_last_activity_ms) > CONFIG_BUDDY_FACE_ASLEEP_S * 1000;
            if (rest != s_face_resting) {
                s_face_resting = rest;
                ESP_LOGI(TAG, "%s", rest ? "the room is quiet; the face rests"
                                         : "something stirred; the face wakes");
                show_state(rest ? "asleep" : "idle");
            }
#endif
            continue;
        }
        if (heard) barge_in(now);
        if (s_barged && since(now, s_barged_ms) > BARGE_MAX_MS) s_barged = false;
        if (s_speaking || frame_rms(in, IN_SAMPLES) > VOICE_RMS) s_last_activity_ms = now;
        if (s_phase == PHASE_OPENING && s_connected) s_phase = PHASE_OPEN;
        const char *why = NULL;
        if (s_phase == PHASE_OPENING && since(now, s_opened_ms) > OPEN_TIMEOUT_MS) why = "never opened";
        else if (s_phase == PHASE_OPEN && !s_connected) why = "dropped";
#if CONFIG_BUDDY_SCREEN_FACE && CONFIG_BUDDY_FACE_ASLEEP_S > 0
        /* A skill said goodbye for the child, so end the conversation rather than
         * only looking like it. Resting the face alone left the socket open for
         * another CONFIG_BUDDY_VOICE_IDLE_S, during which the kit still answered
         * anything said near it — a Buddy that reads as asleep and replies anyway,
         * under a pill that now said "Say Jarvis" and did not mean it.
         *
         * Once it has stopped speaking: the goodbye is the last thing it says, and
         * cutting the session mid-sentence would take the words with it. */
        else if (s_face_rest_held && !s_speaking) why = "said goodbye";
#endif
        else if (since(now, s_last_activity_ms) > CONFIG_BUDDY_VOICE_IDLE_S * 1000) why = "quiet";
        if (why) { session_end(why); continue; }
        f.gen = s_gen;
#endif

        /* Half duplex, for now. While the Buddy talks, the microphone hears the
         * speaker, and omni would take its own voice for the child's and interrupt
         * itself. Silence keeps the stream continuous — omni wants it gapless. Saying
         * the wake word is how a child gets a word in meanwhile.
         *
         * With echo cancellation (BUDDY_AEC) the speaker is already out of the signal,
         * so the child is heard while the Buddy talks — unless
         * BUDDY_AEC_KEEP_HALF_DUPLEX asks for the old behaviour. */
#if !CONFIG_BUDDY_AEC || CONFIG_BUDDY_AEC_KEEP_HALF_DUPLEX
        if (s_speaking) memset(in, 0, IN_SAMPLES * sizeof(int16_t));
#endif

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
    EXT_RAM_BSS_ATTR static int16_t up[UP_SAMPLES];      /* PSRAM, like this task's stack */
    static up_pkt_t p;
    int16_t hist[VOICE_UP_PHASE] = { 0 };
    uint64_t us_fir = 0, us_enc = 0;
    uint32_t n = 0;
    for (;;) {
        xQueueReceive(s_pcmq, &f, portMAX_DELAY);

        /* Loud enough for the far end, before anything resamples or encodes it. */
        uplink_gain(f.pcm, IN_SAMPLES);

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

/* omni to the amp. */
static void down_task(void *arg)
{
    (void)arg;
    static pkt_t p;
    bool was_speaking = false;           /* owned by this task: lip-sync state is its own */
    EXT_RAM_BSS_ATTR static int16_t pcm[960];   /* up to 60 ms at 16 kHz; PSRAM, like the stack */
    uint64_t us_dec = 0;                 /* decode total, for the log */
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
            ESP_LOGI(TAG, "down: held %u, shed %lu silence + %lu old, %lu waited-for, %lu underruns, "
                     "%lu flushes dropping %lu",
                     (unsigned)uxQueueMessagesWaiting(s_down), (unsigned long)s_shed_silence,
                     (unsigned long)s_shed_old, (unsigned long)s_late, (unsigned long)s_underruns,
                     (unsigned long)s_flushes, (unsigned long)s_flush_dropped);
            ESP_LOGI(TAG, "down: per 20 ms packet: opus decode %llu us", us_dec / 500);
            us_dec = 0;
        }
        esp_audio_dec_in_raw_t raw = { .buffer = p.data, .len = p.len };
        esp_audio_dec_out_frame_t fo = { .buffer = (uint8_t *)pcm, .len = sizeof(pcm) };
        esp_audio_dec_info_t info;
        int64_t t0 = esp_timer_get_time();
        if (esp_opus_dec_decode(s_dec, &raw, &fo, &info) != ESP_AUDIO_ERR_OK) continue;
        us_dec += esp_timer_get_time() - t0;
        size_t ns = fo.decoded_size / sizeof(int16_t);
        /* The mouth hears exactly what the speaker is about to: this PCM, never the
         * microphone and never omni's text. Fed only while the Buddy is speaking, so
         * silence between turns gives the mouth back to the authored motion rather
         * than holding it shut. */
        bool sp = s_speaking;
        if (sp && !was_speaking) mouth_start();
        if (!sp && was_speaking) mouth_stop();
        if (sp) mouth_feed(pcm, ns);
        was_speaking = sp;
        audio_write(pcm, ns);
#if CONFIG_BUDDY_SCREEN_FACE
        /* This frame is now the speaker's problem, so it counts as heard. Then wear
         * anything the child has reached — more than one, if a short tag ran past. */
        s_play_ms += (uint32_t)(ns / (AUDIO_SAMPLE_RATE / 1000));
        expr_due_t d;
        while (s_expr_q && xQueuePeek(s_expr_q, &d, 0) == pdTRUE && d.at_ms <= s_play_ms) {
            xQueueReceive(s_expr_q, &d, 0);
            if (face_expression_wear(d.id))
                ESP_LOGI(TAG, "face: %s (due %lu ms, played %lu)", d.id,
                         (unsigned long)d.at_ms, (unsigned long)s_play_ms);
            else
                ESP_LOGW(TAG, "no expression '%s' in this firmware", d.id);
        }
#endif
    }
}

#if CONFIG_BUDDY_SCREEN_FACE
void voice_face_rest(bool rest)
{
    /* Recorded here and acted on by up_task, which owns the face's idle behaviour.
     * Called from the MQTT task, which must not touch LVGL. */
    s_face_rest_held = rest;
    if (!rest) s_last_activity_ms = now_ms();   /* woken: start the quiet clock again */
    ESP_LOGI(TAG, "a skill asks the face to %s", rest ? "settle down" : "wake up");
}
#endif

#define VOICE_PATH "/v1/stem/voice/stream"

bool voice_start(const char *serial, const char *gateway, const char *user_token)
{
#if CONFIG_BUDDY_SCREEN_FACE
    if (!s_expr_q) s_expr_q = xQueueCreate(EXPR_PENDING, sizeof(expr_due_t));
#endif
    if (s_ws) return true;
    if (!user_token || !user_token[0]) {
        ESP_LOGE(TAG, "no user token: omni resolves the persona with it (set the kit up "
                      "from the website, or CONFIG_BUDDY_VOICE_TOKEN on the bench)");
        return false;
    }
    if (!gateway || !gateway[0]) { ESP_LOGE(TAG, "no gateway host to talk to"); return false; }

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
#if CONFIG_BUDDY_UPLINK_GAIN_DB > 0
    /* Computed once: powf on every 20 ms frame would be absurd, and the figure is
     * fixed at build time anyway. Logged because a kit that is too quiet for the
     * cloud and a kit that is clipping look identical from here. */
    s_up_gain = (int32_t)(powf(10.0f, CONFIG_BUDDY_UPLINK_GAIN_DB / 20.0f) * (1 << UP_GAIN_Q) + 0.5f);
    ESP_LOGI(TAG, "uplink gain: +%d dB (x%.2f) on the audio omni hears",
             CONFIG_BUDDY_UPLINK_GAIN_DB, (double)s_up_gain / (1 << UP_GAIN_Q));
#endif

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
    size_t ulen = strlen(gateway) + strlen(VOICE_PATH) + strlen(serial) + strlen(user_token) + 40;
    char *url = heap_caps_malloc(ulen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    snprintf(url, ulen, "wss://%s" VOICE_PATH "?serial=%s&token=%s", gateway, serial, user_token);

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
    wc.cert_pem = isrg_root_x1_pem_start;
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
    ESP_LOGI(TAG, "listening for the wake word; will talk to wss://%s" VOICE_PATH " as %s", gateway, serial);
    status_set(STATUS_IDLE);
    /* From here the face belongs to the conversation, not to the demo cycle. A kit
     * that never gets online keeps cycling, which is the point of that cycle. */
    show_state("idle");
    return true;
#else
    esp_wifi_set_ps(WIFI_PS_NONE);       /* always in a conversation: see session_begin */
    ESP_LOGI(TAG, "connecting to wss://%s" VOICE_PATH " as %s", gateway, serial);
    status_set(STATUS_CONNECTING);
    return esp_websocket_client_start(s_ws) == ESP_OK;
#endif
}

#else   /* voice off: the rest of the firmware asks, and is told no */

bool voice_start(const char *serial, const char *gateway, const char *user_token)
{
    (void)serial; (void)gateway; (void)user_token;
    return false;
}
bool voice_active(void) { return false; }

#endif
