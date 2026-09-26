/*
 * Hold the firmware to the website's answers.
 *
 * Two generated goldens, both emitted by tools/bake_face.ts from the front end's own
 * TypeScript, so the expectation cannot drift from the thing it is generated beside:
 *
 *   main/generated/xcheck.txt          — sampleMotion(), per layer, per instant
 *   main/generated/lipsync-xcheck.txt  — classify() + VisemeSmoother over real speech
 *
 * Both matter for the same reason. The studio shows a child their Buddy blinking and
 * talking; the kit has to be the same Buddy. A face that moves or lip-syncs
 * differently on the device is a bug only a child would find, and only by being
 * disappointed.
 *
 * Built for the host, not the device — there is no LVGL and no panel here, only the
 * arithmetic. Run it inside the container that has gcc:
 *
 *   docker run --rm -v "$PWD":/fw -w /fw espressif/idf:v5.2.2 bash tools/xcheck.sh
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "face_motion.h"
#include "face_lipsync.h"

static int fails;

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("  FAIL ", stdout);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fails++;
}

/* Floats that came from a JS double and a C float will not be bit-equal, and
 * demanding that would only teach us to loosen it later. A 1/1000 face unit is far
 * below a pixel on a 240 px panel. */
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)

static int state_index(const char *name)
{
    for (int i = 0; i < face_state_count; i++)
        if (!strcmp(face_states[i].name, name)) return i;
    return -1;
}

static void check_motion(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fail("cannot open %s", path); return; }

    char st[32];
    unsigned t;
    int layer, sprite, rows = 0;
    float x, y, sc, scy, rot, opa;
    while (fscanf(f, "%31s %u %d %f %f %f %f %f %f %d",
                  st, &t, &layer, &x, &y, &sc, &scy, &rot, &opa, &sprite) == 10) {
        int s = state_index(st);
        if (s < 0) { fail("unknown state %s", st); break; }
        face_values_t v[FACE_LAYER_COUNT];
        face_motion_sample(s, t, v);
        const face_values_t *g = &v[layer];
        if (!NEAR(g->x, x) || !NEAR(g->y, y) || !NEAR(g->scale, sc) || !NEAR(g->scale_y, scy)
            || !NEAR(g->rotation, rot) || !NEAR(g->opacity, opa) || g->sprite != sprite)
            fail("%s t=%u layer=%d: got %.4f %.4f %.4f %.4f %.4f %.4f %d, want %.4f %.4f %.4f %.4f %.4f %.4f %d",
                 st, t, layer, g->x, g->y, g->scale, g->scale_y, g->rotation, g->opacity, g->sprite,
                 x, y, sc, scy, rot, opa, sprite);
        rows++;
    }
    fclose(f);
    printf("motion: %d rows checked\n", rows);
}

static const char *VISEME_NAME[8] = { "rest", "mbp", "small", "mid", "wide", "round", "pucker", "teeth" };

static void check_lipsync(const char *pcm_path, const char *golden_path)
{
    FILE *pf = fopen(pcm_path, "rb");
    if (!pf) { fail("cannot open %s", pcm_path); return; }
    static int16_t pcm[1 << 20];
    size_t n = fread(pcm, sizeof(int16_t), sizeof(pcm) / sizeof(int16_t), pf);
    fclose(pf);

    FILE *gf = fopen(golden_path, "r");
    if (!gf) { fail("cannot open %s", golden_path); return; }

    /* Fed in chunks that divide neither the frame nor the hop, on purpose: the
     * decoder on the device will not hand over tidy 256-sample buffers, and framing
     * that quietly depends on the buffer size is exactly the kind of bug that only
     * shows up once there is audio hardware in the loop. */
    const size_t CHUNK = 173;
    size_t at = 0;
    int k, want_raw, want, checked = 0, agreed = 0;
    unsigned want_t;

    face_lipsync_start();
    while (fscanf(gf, "%d %u %d %d", &k, &want_t, &want_raw, &want) == 4) {
        /* Feed up to the end of the frame this golden row describes. */
        size_t need = (size_t)face_viseme_frame + (size_t)k * face_viseme_hop;
        if (need > n) break;
        while (at < need) {
            size_t take = need - at < CHUNK ? need - at : CHUNK;
            face_lipsync_feed(&pcm[at], take);
            at += take;
        }
        if (face_lipsync_frames() != k + 1)
            fail("frame %d: firmware classified %d frames, golden has %d",
                 k, face_lipsync_frames(), k + 1);
        int got = face_lipsync_viseme();
        checked++;
        if (got == want) agreed++;
        else fail("frame %d (t=%u ms): got %s, want %s", k, want_t, VISEME_NAME[got], VISEME_NAME[want]);
        /* The renderer is handed a sprite, not a class: feeding nothing returns the
         * same answer, and it has to be the one the baked table names. */
        int16_t sp = face_lipsync_feed(NULL, 0);
        if (sp != face_viseme_sprite[got])
            fail("frame %d: sprite %d for %s, table says %d", k, sp, VISEME_NAME[got], face_viseme_sprite[got]);
    }
    fclose(gf);
    printf("lipsync: %d frames checked, %d agreed (%.1f%%)\n",
           checked, agreed, checked ? 100.0 * agreed / checked : 0.0);

    /* And the mouth must shut when the audio does, without waiting out a hold. */
    face_lipsync_stop();
    if (face_lipsync_viseme() != 0) fail("stop() left the mouth on %s", VISEME_NAME[face_lipsync_viseme()]);
}

int main(int argc, char **argv)
{
    const char *gen = argc > 1 ? argv[1] : "main/generated";
    char path[512];

    snprintf(path, sizeof(path), "%s/xcheck.txt", gen);
    check_motion(path);

    snprintf(path, sizeof(path), "%s/lipsync-xcheck.txt", gen);
    check_lipsync(argc > 2 ? argv[2] : "tools/lipsync-fixture.pcm", path);

    printf(fails ? "\n%d FAILURES\n" : "\nall checks passed\n", fails);
    return fails ? 1 : 0;
}
