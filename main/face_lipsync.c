#include "face_lipsync.h"
#include "generated/face_assets.h"
#include <math.h>
#include <string.h>

/* Viseme order matches face_viseme_sprite and visemes.ts. */
enum { V_REST = 0, V_MBP, V_SMALL, V_MID, V_WIDE, V_ROUND, V_PUCKER, V_TEETH };

#define MAXN 512                      /* face_viseme_frame; static, never malloc'd */

static float s_re[MAXN], s_im[MAXN], s_win[MAXN];
/* A ring, not a sliding array. The first version memmove'd 511 samples for every
 * sample that arrived — 16 MB/s of copying at this rate — and the first run on real
 * silicon measured the whole classifier at 21.4 ms of CPU per 16 ms of audio, which
 * is more than a core. Writing one sample and walking the ring at hop boundaries is
 * the same arithmetic in the same order, so the conformance golden does not move. */
static int16_t s_ring[MAXN];
static int s_w;                       /* where the next sample goes */
static int s_fill;                    /* samples held, capped at the frame length */
static int s_since_hop;
static float s_peak;                  /* loudness reference for this voice */
static int s_cur, s_cand, s_votes;
static uint32_t s_since_ms;
static int s_frames;
static int s_win_ready;

static void make_window(int n)
{
    for (int i = 0; i < n; i++) s_win[i] = 0.5f - 0.5f * cosf((2.0f * (float)M_PI * i) / (n - 1));
    s_win_ready = 1;
}

/* In-place iterative radix-2 FFT, written out rather than pulled in so it matches
 * the one the browser runs bit for bit in structure. */
static void fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                float ar = re[i + k], ai = im[i + k];
                float br = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                float bi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ar + br; im[i + k] = ai + bi;
                re[i + k + len / 2] = ar - br; im[i + k + len / 2] = ai - bi;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
}

static int bin_of(int hz, int n)
{
    int b = (int)lroundf((float)hz * n / (float)face_viseme_rate);
    if (b < 0) b = 0;
    if (b > n / 2 - 1) b = n / 2 - 1;
    return b;
}

static float band(const float *re, const float *im, int n, int lo, int hi)
{
    float e = 0;
    for (int b = bin_of(lo, n); b <= bin_of(hi, n); b++) e += re[b] * re[b] + im[b] * im[b];
    return e;
}

/** One frame's shape. `rel` is loudness relative to this voice's recent peak, `zcr`
 *  the zero-crossing rate measured while the frame was read out of the ring. */
static int classify_frame(int n, float rel, float zcr)
{
    const face_viseme_table_t *t = &face_viseme_table;
    const face_viseme_bands_t *b = &face_viseme_bands;

    if (rel < t->silence_rms) return V_REST;

    float f1lo = band(s_re, s_im, n, b->f1lo_a, b->f1lo_b);
    float f1hi = band(s_re, s_im, n, b->f1hi_a, b->f1hi_b);
    float f2   = band(s_re, s_im, n, b->f2_a, b->f2_b);
    float sib  = band(s_re, s_im, n, b->sib_a, b->sib_b);
    float total = f1lo + f1hi + f2 + sib + 1e-12f;

    /* Flatness over the upper half only. Down where the fundamental lives
     * everything is peaky, so including it would call every frame harmonic. */
    int lo = bin_of(1000, n), hi = bin_of(7800, n);
    float log_sum = 0, lin_sum = 0;
    int cnt = 0;
    for (int i = lo; i <= hi; i++) {
        float p = s_re[i] * s_re[i] + s_im[i] * s_im[i] + 1e-20f;
        log_sum += logf(p); lin_sum += p; cnt++;
    }
    float flat = cnt ? expf(log_sum / cnt) / (lin_sum / cnt + 1e-20f) : 0.0f;

    /* Order is load-bearing: fricatives first, because /s/ is noise and would
     * otherwise read as some vowel; closures next, because on the bands alone a
     * bilabial murmur looks like a very low back vowel — and getting *that* wrong
     * costs more than getting any vowel wrong. */
    if (zcr > t->teeth_zcr && flat > t->teeth_flat) return V_TEETH;

    float low = f1lo / total;
    if (rel < t->mbp_rms && low > t->mbp_low) return V_MBP;

    float open = f1hi / (f1lo + f1hi + 1e-12f);
    float front = f2 / (f1lo + f1hi + f2 + 1e-12f);
    if (front > t->narrow_front) return open > t->mid_open ? V_MID : V_SMALL;
    if (open > t->wide_open && rel > t->wide_rms) return V_WIDE;
    if (open > t->mid_open) return V_MID;
    return low > t->pucker_low ? V_PUCKER : V_ROUND;
}

void face_lipsync_start(void)
{
    s_fill = 0; s_w = 0; s_since_hop = 0; s_peak = 0;
    s_cur = V_REST; s_cand = V_REST; s_votes = 0; s_since_ms = 0; s_frames = 0;
}

void face_lipsync_stop(void)
{
    face_lipsync_start();
}

int16_t face_lipsync_feed(const int16_t *pcm, size_t samples)
{
    const int n = face_viseme_frame > MAXN ? MAXN : face_viseme_frame;
    if (!s_win_ready) make_window(n);

    for (size_t i = 0; i < samples; i++) {
        s_ring[s_w] = pcm[i];
        if (++s_w == n) s_w = 0;
        if (s_fill < n) {
            if (++s_fill < n) continue;     /* first full frame: classify it now */
        } else if (++s_since_hop < face_viseme_hop) {
            continue;
        }
        s_since_hop = 0;
        s_frames++;

        /* One pass over the ring, oldest sample first, doing everything the frame
         * needs: loudness, zero crossings, and the windowed input to the FFT. The
         * order is the order the front end reads its frame in, which is what keeps
         * the two implementations answering the same. */
        int idx = s_w;                      /* a full ring: the next write is the oldest */
        int crossings = 0;
        int16_t prev = 0;
        float sum = 0;
        for (int k = 0; k < n; k++) {
            int16_t raw = s_ring[idx];
            if (++idx == n) idx = 0;
            if (k > 0 && ((raw < 0) != (prev < 0))) crossings++;
            prev = raw;
            float v = raw / 32768.0f;
            sum += v * v;
            s_re[k] = v * s_win[k];
            s_im[k] = 0.0f;
        }
        fft(s_re, s_im, n);

        /* Relative loudness, with a slowly decaying peak so a long pause does not
         * drag the reference down into the noise floor and find speech in it. */
        float rms = sqrtf(sum / n);
        s_peak = fmaxf(rms, s_peak * 0.995f);
        float rel = s_peak > 1e-5f ? fminf(1.0f, rms / s_peak) : 0.0f;

        int v = classify_frame(n, rel, (float)crossings / (n - 1));

        /* The clock is the audio's own, not the system's: a frame is worth
         * face_viseme_hop samples however late the decoder delivered it, and the
         * minimum hold is about how long a shape is *heard*, not waited for. It also
         * makes the whole thing reproducible off-device, which is how it is tested. */
        uint32_t now_ms = (uint32_t)((uint64_t)(n + (s_frames - 1) * face_viseme_hop)
                                     * 1000u / (uint32_t)face_viseme_rate);

        /* A closure is allowed to interrupt: lip closures are short, and holding the
         * previous shape through an "m" is the error the class exists to fix. */
        int urgent = (v == V_MBP || v == V_REST);
        if (v == s_cur) {
            s_cand = v; s_votes = 0;
        } else {
            if (v == s_cand) s_votes++; else { s_cand = v; s_votes = 1; }
            if ((s_votes >= face_viseme_table.switch_frames || urgent)
                && (urgent || now_ms - s_since_ms >= (uint32_t)face_viseme_table.min_hold_ms)) {
                s_cur = v; s_since_ms = now_ms; s_votes = 0;
            }
        }
    }
    return face_viseme_sprite[s_cur];
}

/** The class currently shown, 0..7. For the conformance check and for logs. */
int face_lipsync_viseme(void) { return s_cur; }

/** How many frames have been classified since the last start(). */
int face_lipsync_frames(void) { return s_frames; }
