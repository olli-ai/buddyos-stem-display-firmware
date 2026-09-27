/*
 * Bake a stem-fe design into C the display firmware can draw.
 *
 * This is the format translation, and the point of the whole exercise. The website
 * holds a face as *choices* over a parts catalogue and motion as keyframes over
 * layers; neither is something a microcontroller can draw. The device draws sprites.
 * So the catalogue's `draw(ctx) => Shape[]` functions — which are TypeScript, and can
 * therefore never be shipped to a device — are run here, once, at build time, and what
 * comes out is rasterised per layer into LVGL images.
 *
 * That is the answer to the question ADR 0009 left open: a generator cannot emit C from
 * a function, so it bakes the function's *output* instead.
 *
 * Needs: rsvg-convert and ImageMagick (`magick`), both on PATH. Run it from the front
 * end, whose tsconfig resolves the `~` imports; the picture loaders are there because
 * looks.ts imports artwork this generator never rasterises.
 *
 *   cd ../buddyos-stem-fe && npx esbuild ../buddyos-stem-display-firmware/tools/bake_face.ts \
 *     --bundle --platform=node --format=cjs --outfile=/tmp/bake.cjs \
 *     --loader:.webp=text --loader:.png=text \
 *   && node /tmp/bake.cjs ../buddyos-stem-display-firmware/main/generated
 */
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { BODY_COLORS, LOOKS } from '../../buddyos-stem-fe/src/data/looks';
import { BROWS, EYES, ALL_MOUTHS, FACE_STATES, defaultFaceStates, browPart, eyePart, mouthPart, type FaceComposition, type FaceState, type Shape, type Paint } from '../../buddyos-stem-fe/src/data/faceParts';
import {
    EASINGS, EASING_LUT, LUT_SIZE, LAYERS, MOTION_PRESETS, NUMERIC_PROPS, RANGE,
    scaleStateMotion, periodOf, isSpriteTrack, sampleMotion,
    type LayerId, type StateMotion, type Track, type NumericTrack, type SpriteTrack,
} from '../../buddyos-stem-fe/src/data/motion';
import {
    BANDS, FRAME, HOP, SAMPLE_RATE, VISEMES, VISEME_TABLE,
    Loudness, VisemeSmoother, classify, features,
    MOUTH_STYLES, partsForStyle, type Viseme,
} from '../../buddyos-stem-fe/src/data/visemes';

const SIZE = 240;                       // the GC9A01 is 240x240
const BASE_ID = 'sprout' as const;

/* The four states the firmware cycles. Deliberately drawn parts only: an 'artwork'
 * part is a reference to the artist's own bitmap, and resolving those through Vite's
 * asset pipeline would prove nothing about translating *shapes*. */
/* All seven the website authors, not the four this started with. A conversation
 * passes through more than four moods, and a state the device does not carry is a
 * state a child can design and never see. */
const STATES = FACE_STATES;
type TestState = FaceState;

/* The built-in face is the front end's own default, imported rather than retyped.
 * A kit nobody has designed for therefore looks exactly like a design nobody has
 * touched, which is the only sane relationship between the two. */
const DESIGN: Record<TestState, FaceComposition> = defaultFaceStates();
/** Which mouth each sound draws. A build-time choice until a design carries it. */
const MOUTH_STYLE = 'natural';

const MOTION_FOR: Record<TestState, string[]> = {
    idle: ['breathe', 'blink'],
    listening: ['browRaise'],
    thinking: ['nod'],
    speaking: ['talk', 'blink'],
    /* Busy is working, not waiting, so it gets the same quiet breath as idle without
     * the blink. Error shakes its head once a cycle. Asleep only breathes — a
     * sleeping Buddy that blinks is awake. */
    busy: ['breathe'],
    error: ['shake'],
    asleep: ['breathe'],
};

// ---------------------------------------------------------------- palette
const base = LOOKS[BASE_ID];
const paint = base.paint!;
const INK = '#2F401E', PINK = '#FFD1D1', TONGUE = '#FF7A8A', WATER = '#BFE4FF';
const patchFill = paint.fill || base.swatch;
const paintOf = (tok?: Paint): string => ({
    ink: paint.ink || INK, white: '#FFFFFF', pink: PINK, tongue: TONGUE,
    water: WATER, accent: '#7CB342', body: patchFill,
}[tok as string] ?? 'none');

// ------------------------------------------------------------ shapes → svg
function shapeToSvg(sh: Shape): string {
    const st = (s: any) => `fill="${paintOf(s.fill)}" stroke="${paintOf(s.stroke)}" stroke-width="${s.w || 0}" opacity="${s.op ?? 1}" stroke-linecap="round" stroke-linejoin="round"`;
    if (sh.t === 'path') return `<path d="${sh.d}" ${st(sh)}/>`;
    if (sh.t === 'ellipse') return `<ellipse cx="${sh.cx}" cy="${sh.cy}" rx="${sh.rx}" ry="${sh.ry}" ${st(sh)}${sh.rot ? ` transform="rotate(${sh.rot} ${sh.cx} ${sh.cy})"` : ''}/>`;
    if (sh.t === 'polygon') return `<polygon points="${sh.pts}" ${st(sh)}/>`;
    return ''; // 'image' shapes are artwork parts, excluded from this design on purpose
}
const svgDoc = (body: string) =>
    `<svg xmlns="http://www.w3.org/2000/svg" width="${SIZE}" height="${SIZE}" viewBox="0 0 100 100">${body}</svg>`;

// ----------------------------------------------------------- rasterising
const tmp = mkdtempSync(join(tmpdir(), 'bake-'));
let n = 0;
/** Render an SVG, trim it to what it actually covers, and return ARGB8888 + placement. */
function raster(svg: string): { w: number; h: number; x: number; y: number; px: Buffer; png: string } {
    const id = `s${n++}`;
    const sv = join(tmp, id + '.svg'), pn = join(tmp, id + '.png'), tr = join(tmp, id + '-t.png');
    writeFileSync(sv, svg);
    execFileSync('rsvg-convert', ['-w', String(SIZE), '-h', String(SIZE), '-o', pn, sv]);
    // Where the ink actually is, so a 30x30 eye is not stored as a 240x240 sheet.
    const box = execFileSync('magick', [pn, '-trim', '-format', '%w %h %X %Y', 'info:']).toString().trim();
    const m = box.match(/^(\d+) (\d+) \+?(-?\d+) \+?(-?\d+)$/);
    if (!m) return { w: 0, h: 0, x: 0, y: 0, px: Buffer.alloc(0), png: '' };
    const [w, h, x, y] = m.slice(1).map(Number);
    execFileSync('magick', [pn, '-trim', '+repage', '-depth', '8', tr]);
    // BGRA is the byte order LVGL's ARGB8888 expects on a little-endian target.
    const rgba = execFileSync('magick', [tr, '-depth', '8', 'RGBA:-'], { maxBuffer: 1 << 28 });
    const px = Buffer.alloc(rgba.length);
    for (let i = 0; i < rgba.length; i += 4) {
        px[i] = rgba[i + 2]; px[i + 1] = rgba[i + 1]; px[i + 2] = rgba[i]; px[i + 3] = rgba[i + 3];
    }
    return { w, h, x, y, px, png: tr };
}

/** The background: the mascot's own render with its painted eyes and mouth patched
 *  out. Kept **transparent outside the silhouette**, as RGB565A8, because that alpha
 *  is what lets the device recolour the body.
 *
 *  The website tints a Buddy by filling the artwork's own mask with a flat colour —
 *  `backgroundColor` under a `maskImage` — so a tinted Buddy is a silhouette, not a
 *  shaded render. LVGL's image recolour at full opacity is the same operation on the
 *  same pixels, which is why the two agree without anyone matching them by eye.
 *  Costs 57,600 B more than the flattened version and buys every body colour. */
function bakeBackground(): { px: Buffer } {
    const src = '/Users/leminhson/olli_projects/buddyos-stem-fe/src/assets/mascots/sprout.webp';
    const k = 1.6; // PATCH_SCALE, as BuddyFace uses it
    const grad = `<defs><radialGradient id="g" cx="0.5" cy="0.5" r="0.5">
        <stop offset="0" stop-color="${patchFill}" stop-opacity="1"/>
        <stop offset="0.88" stop-color="${patchFill}" stop-opacity="1"/>
        <stop offset="1" stop-color="${patchFill}" stop-opacity="0"/></radialGradient></defs>`;
    const ells = [
        ...paint.eyes.map(([cx, cy]: number[]) => ({ cx, cy, rx: paint.eyeR[0] * k, ry: paint.eyeR[1] * k })),
        { cx: paint.mouth[0], cy: paint.mouth[1], rx: paint.mouthR[0] * k, ry: paint.mouthR[1] * k },
    ].map((e) => `<ellipse cx="${e.cx}" cy="${e.cy}" rx="${e.rx}" ry="${e.ry}" fill="url(#g)"/>`).join('');
    const pf = join(tmp, 'patch.png'), bg = join(tmp, 'bg.png');
    writeFileSync(join(tmp, 'patch.svg'), svgDoc(grad + ells));
    execFileSync('rsvg-convert', ['-w', String(SIZE), '-h', String(SIZE), '-o', pf, join(tmp, 'patch.svg')]);
    execFileSync('magick', ['-size', `${SIZE}x${SIZE}`, 'xc:none', '(', src, '-resize', `${SIZE}x${SIZE}`, ')',
        '-gravity', 'center', '-composite', pf, '-gravity', 'center', '-composite', bg]);
    const rgba = execFileSync('magick', [bg, '-depth', '8', 'RGBA:-'], { maxBuffer: 1 << 28 });
    /* RGB565A8 as LVGL stores it: the whole colour plane, then the whole alpha
     * plane. Not interleaved — a per-pixel 3-byte layout renders as noise. */
    const px = Buffer.alloc(SIZE * SIZE * 3);
    const alpha = SIZE * SIZE * 2;
    for (let i = 0, p = 0; i < rgba.length; i += 4, p++) {
        const v = ((rgba[i] & 0xf8) << 8) | ((rgba[i + 1] & 0xfc) << 3) | (rgba[i + 2] >> 3);
        px[p * 2] = v & 0xff; px[p * 2 + 1] = v >> 8;
        px[alpha + p] = rgba[i + 3];
    }
    return { px };
}

// --------------------------------------------------- which sprites are needed
const eyeCtx = (side: -1 | 1) => {
    const r = paint.eyeDraw || 7;
    const [cx, cy] = side < 0 ? paint.eyes[0] : paint.eyes[1];
    return { cx, cy, r, side };
};
const mouthCtx = () => paint.mouthDraw || { cx: 50, cy: 66, rx: 8, ry: 4.5 };

function shapesFor(layer: LayerId, partId: string): Shape[] {
    if (layer === 'mouth') return mouthPart(partId).draw(mouthCtx() as any);
    const side = layer.endsWith('R') ? 1 : -1;
    const ctx = eyeCtx(side as -1 | 1);
    return (layer.startsWith('brow') ? browPart(partId) : eyePart(partId)).draw(ctx as any);
}

const motions: Record<TestState, StateMotion | undefined> = {} as any;
for (const st of STATES) {
    let acc: StateMotion | undefined;
    for (const id of MOTION_FOR[st]) {
        const p = MOTION_PRESETS.find((x) => x.id === id)!;
        const made = scaleStateMotion(p.build(DESIGN[st]), 1, 1);
        if (!acc) acc = { duration: made.period, loop: true, tracks: made.tracks };
        else acc = { duration: Math.max(acc.duration, made.period), loop: true,
                     tracks: [...acc.tracks.map((t) => ({ ...t, every: periodOf(t, acc!.duration) })), ...made.tracks] };
    }
    motions[st] = acc;
}

/** Every (layer, part) the four states can show, including mid-animation swaps —
 *  and every mouth the chosen talk style can ask for, which the states alone would
 *  not cover. */
const needed = new Map<string, { layer: LayerId; part: string }>();
const putNeeded = (layer: LayerId, part: string) => needed.set(`${layer}:${part}`, { layer, part });
/* Every style's mouths, not just the default one's: which style a face talks in
 * arrives with the design at runtime. */
for (const st of MOUTH_STYLES)
    for (const v of VISEMES) {
        const part = partsForStyle(st.id)[v];
        if (part) putNeeded('mouth', part);
    }
for (const st of STATES) {
    const d = DESIGN[st];
    const put = putNeeded;
    put('browL', d.brows); put('browR', d.brows);
    put('eyeL', d.eyes); put('eyeR', d.eyes); put('mouth', d.mouth);
    for (const t of motions[st]?.tracks || []) {
        if (isSpriteTrack(t)) for (const k of t.keys) put(t.layer, k.id);
    }
}

/* ...and then the whole catalogue, because a design arrives from the website at
 * runtime and may name any part in it. Baking only what the built-in states use
 * would mean every second face a child made fell back to a default — which is ADR
 * 0009's rule working correctly and looking exactly like a bug.
 *
 * 'artwork' is skipped: it is a reference to an artist's bitmap resolved through
 * Vite, not a drawing, and there is nothing here to rasterise. */
for (const e of EYES) if (e.id !== 'artwork') { putNeeded('eyeL', e.id); putNeeded('eyeR', e.id); }
for (const b of BROWS) { putNeeded('browL', b.id); putNeeded('browR', b.id); }
for (const m of ALL_MOUTHS) if (m.id !== 'artwork') putNeeded('mouth', m.id);

// --------------------------------------------------------------- emit C
const out = process.argv[2] || '.';
mkdirSync(out, { recursive: true });
const carr = (b: Buffer) => {
    const parts: string[] = [];
    for (let i = 0; i < b.length; i += 16) parts.push('    ' + [...b.subarray(i, i + 16)].map((v) => '0x' + v.toString(16).padStart(2, '0')).join(', '));
    return parts.join(',\n');
};

const sprites: { sym: string; layer: LayerId; part: string; w: number; h: number; x: number; y: number; png?: string }[] = [];
let cs = `/* GENERATED by tools/bake_face.ts — do not edit. */\n#include "face_assets.h"\n\n`;

for (const [key, { layer, part }] of needed) {
    const shapes = shapesFor(layer, part).filter((s) => s.t !== 'image');
    const sym = 'spr_' + key.replace(/[^a-zA-Z0-9]/g, '_');
    if (!shapes.length) { sprites.push({ sym, layer, part, w: 0, h: 0, x: 0, y: 0 }); continue; }
    const r = raster(svgDoc(shapes.map(shapeToSvg).join('')));
    if (!r.w) { sprites.push({ sym, layer, part, w: 0, h: 0, x: 0, y: 0 }); continue; }
    cs += `static const uint8_t ${sym}_map[] = {\n${carr(r.px)}\n};\n`;
    cs += `const lv_image_dsc_t ${sym} = { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = ${r.w}, .h = ${r.h}, .stride = ${r.w * 4} }, .data_size = sizeof(${sym}_map), .data = ${sym}_map };\n\n`;
    sprites.push({ sym, layer, part, w: r.w, h: r.h, x: r.x, y: r.y, png: r.png });
}

const bgd = bakeBackground();
const BG_PNG = join(tmp, 'bg.png');
cs += `static const uint8_t face_bg_map[] = {\n${carr(bgd.px)}\n};\n`;
cs += `const lv_image_dsc_t face_bg = { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8, .w = ${SIZE}, .h = ${SIZE}, .stride = ${SIZE * 2} }, .data_size = sizeof(face_bg_map), .data = face_bg_map };\n\n`;

/* The body colours the website offers, so a design naming one can be honoured
 * without the firmware keeping its own copy of the palette. `original` is the
 * mascot's own shading and is drawn, not tinted. */
cs += `const face_body_color_t face_body_colors[] = {\n`;
for (const c of BODY_COLORS)
    cs += `    { "${c.id}", 0x${(c.hex || '#000000').slice(1).toUpperCase()} },\n`;
cs += `};\nconst int face_body_color_count = ${BODY_COLORS.length};\n\n`;

cs += `const face_sprite_t face_sprites[] = {\n`;
for (const s of sprites) cs += `    { "${s.layer}", "${s.part}", ${s.w ? '&' + s.sym : 'NULL'}, ${s.x}, ${s.y} },\n`;
cs += `};\nconst int face_sprite_count = ${sprites.length};\n\n`;

const spriteIndex = (layer: LayerId, part: string) => sprites.findIndex((s) => s.layer === layer && s.part === part);
const layerIdx = (l: LayerId) => LAYERS.indexOf(l);
const propIdx = (p: string) => (p === 'sprite' ? 255 : NUMERIC_PROPS.indexOf(p as any));
const FP = { x: 16, y: 16, scale: 1024, scaleY: 1024, rotation: 64, opacity: 255 } as Record<string, number>;

cs += `static const face_key_t keys_all[] = {\n`;
const trackRows: string[] = [];
const stateRows: string[] = [];
let ki = 0;
for (const st of STATES) {
    const m = motions[st];
    const t0 = trackRows.length;
    for (const tr of (m?.tracks || [])) {
        const start = ki;
        for (const k of tr.keys as any[]) {
            const v = isSpriteTrack(tr) ? spriteIndex(tr.layer, k.id) : Math.round(k.v * FP[(tr as NumericTrack).prop]);
            const e = isSpriteTrack(tr) ? 0 : EASINGS.indexOf((k as any).e);
            cs += `    { ${k.t}, ${v}, ${e} },\n`; ki++;
        }
        trackRows.push(`    { ${layerIdx(tr.layer)}, ${propIdx(tr.prop)}, ${start}, ${ki - start}, ${periodOf(tr, m!.duration)} },`);
    }
    stateRows.push(`    { "${st}", ${m?.duration ?? 0}, ${t0}, ${trackRows.length - t0} },`);
}
cs += `};\nconst face_key_t *face_keys = keys_all;\n\n`;
cs += `const face_track_t face_tracks[] = {\n${trackRows.join('\n')}\n};\n\n`;
cs += `const face_state_t face_states[] = {\n${stateRows.join('\n')}\n};\nconst int face_state_count = ${STATES.length};\n\n`;

cs += `const face_layer_part_t face_design[][5] = {\n`;
for (const st of STATES) {
    const d = DESIGN[st];
    const row = (['browL', 'eyeL', 'browR', 'eyeR', 'mouth'] as LayerId[])
        .map((l) => spriteIndex(l, l === 'mouth' ? d.mouth : l.startsWith('brow') ? d.brows : d.eyes));
    cs += `    { ${row.map((i) => `{ ${i} }`).join(', ')} },\n`;
}
cs += `};\n\n`;

cs += `const uint16_t face_easing_lut[${EASINGS.length}][${LUT_SIZE}] = {\n`;
for (const e of EASINGS) cs += `    { ${[...EASING_LUT[e]].join(', ')} },\n`;
cs += `};\n`;

/* The lip-sync classifier's vocabulary, so the firmware reads the same numbers the
 * browser does rather than a second copy someone kept in step by hand. */
const vis = (v: Viseme) => {
    const part = partsForStyle(MOUTH_STYLE)[v];
    return part ? sprites.findIndex((s) => s.layer === 'mouth' && s.part === part) : -1;
};
cs += `/* lip-sync: style "${MOUTH_STYLE}" */\n`;
cs += `const int16_t face_viseme_sprite[${VISEMES.length}] = { ${VISEMES.map(vis).join(', ')} };\n`;
cs += `const face_viseme_bands_t face_viseme_bands = { ${BANDS.f1lo[0]}, ${BANDS.f1lo[1]}, ${BANDS.f1hi[0]}, ${BANDS.f1hi[1]}, ${BANDS.f2[0]}, ${BANDS.f2[1]}, ${BANDS.sib[0]}, ${BANDS.sib[1]} };\n`;
cs += `const face_viseme_table_t face_viseme_table = {\n`
    + `    .silence_rms = ${VISEME_TABLE.silence_rms}f, .wide_rms = ${VISEME_TABLE.wide_rms}f,\n`
    + `    .mbp_rms = ${VISEME_TABLE.mbp_rms}f, .teeth_zcr = ${VISEME_TABLE.teeth_zcr}f,\n`
    + `    .teeth_flat = ${VISEME_TABLE.teeth_flat}f, .narrow_front = ${VISEME_TABLE.narrow_front}f,\n`
    + `    .wide_open = ${VISEME_TABLE.wide_open}f, .mid_open = ${VISEME_TABLE.mid_open}f,\n`
    + `    .mbp_low = ${VISEME_TABLE.mbp_low}f, .pucker_low = ${VISEME_TABLE.pucker_low}f,\n`
    + `    .switch_frames = ${VISEME_TABLE.switch_frames}, .min_hold_ms = ${VISEME_TABLE.min_hold_ms},\n`
    + `};\n`;
/* The style table, so a design can say how its Buddy talks and the device can
 * honour it without a firmware release. */
cs += `const face_mouth_style_t face_mouth_styles[] = {\n`;
for (const st of MOUTH_STYLES) {
    const parts = partsForStyle(st.id);
    const row = VISEMES.map((v) => {
        const part = parts[v];
        return part ? sprites.findIndex((s) => s.layer === 'mouth' && s.part === part) : -1;
    });
    cs += `    { "${st.id}", { ${row.join(', ')} } },\n`;
}
cs += `};\nconst int face_mouth_style_count = ${MOUTH_STYLES.length};\n`;
cs += `const int face_viseme_frame = ${FRAME};\nconst int face_viseme_hop = ${HOP};\n`;
cs += `const int face_viseme_rate = ${SAMPLE_RATE};\n`;
/* Which look this artwork *is*. A design names a base and a colour, and the sprites
 * here were rasterised for exactly one of each — so the device can at least know when
 * it has been asked for something it does not have. */
cs += `const char face_look_base[] = "${BASE_ID}";\n`;
cs += `const char face_look_color[] = "original";\n`;

writeFileSync(join(out, 'face_assets.c'), cs);

/* The same motions sampled here, in the same order, as the golden answer the C
 * interpreter must reproduce. Emitted from the generator rather than written by hand,
 * so the expectation cannot drift from the thing it is generated beside. */
{
    const TIMES = [0, 60, 120, 190, 240, 360, 400, 480, 700, 1000, 1500, 2000, 3000, 3700, 3900];
    const rows: string[] = [];
    for (const st of STATES) {
        const m = motions[st];
        for (const t of TIMES) {
            const v = sampleMotion(m, t);
            for (const l of LAYERS) {
                const lv = v[l];
                const sprite = lv.sprite === undefined ? -1 : spriteIndex(l, lv.sprite);
                rows.push(`${st} ${t} ${LAYERS.indexOf(l)} ${lv.x.toFixed(4)} ${lv.y.toFixed(4)} ${lv.scale.toFixed(4)} ${lv.scaleY.toFixed(4)} ${lv.rotation.toFixed(4)} ${lv.opacity.toFixed(4)} ${sprite}`);
            }
        }
    }
    writeFileSync(join(out, 'xcheck.txt'), rows.join('\n') + '\n');
    console.log(`xcheck.txt: ${rows.length} golden rows`);
}

/* The same thing for lip-sync: the front end's own classifier run over a real clip,
 * frame by frame, as the answer main/face_lipsync.c has to reproduce.
 *
 * The fixture is 1.2 s of Vietnamese followed by 1.2 s of English from the Omnivoice
 * voices this product actually ships, spliced together — because code-switching
 * inside one utterance is the case the whole audio-driven design exists for, and a
 * classifier that only holds up on one language would pass a monolingual fixture.
 *
 * If the two sides ever disagree, the same face lip-syncs differently in the studio
 * and on the kit, and a child would be the one to find it. */
{
    /* Relative to the output directory, not to this file: esbuild bundles this
     * script to a temporary path, so __dirname is not the repo. */
    const pcm = readFileSync(process.argv[3] || join(out, '..', '..', 'tools', 'lipsync-fixture.pcm'));
    const n = Math.floor(pcm.length / 2);
    const samples = new Float32Array(n);
    for (let i = 0; i < n; i++) samples[i] = pcm.readInt16LE(i * 2) / 32768;

    const loud = new Loudness();
    const smoother = new VisemeSmoother();
    const rows: string[] = [];
    const seen = new Map<Viseme, number>();
    for (let off = 0, k = 0; off + FRAME <= n; off += HOP, k++) {
        const f = features(samples.subarray(off, off + FRAME), SAMPLE_RATE);
        const raw = classify({ ...f, rms: loud.push(f.rms) });
        /* The frame's own clock, counted in samples: the firmware has no wall clock
         * it could agree with, and a shape's minimum hold is about how long it is
         * *heard*. */
        const t = Math.floor(((off + FRAME) * 1000) / SAMPLE_RATE);
        const v = smoother.push(raw, t);
        seen.set(v, (seen.get(v) || 0) + 1);
        rows.push(`${k} ${t} ${VISEMES.indexOf(raw)} ${VISEMES.indexOf(v)}`);
    }
    writeFileSync(join(out, 'lipsync-xcheck.txt'), rows.join('\n') + '\n');
    console.log(`lipsync-xcheck.txt: ${rows.length} frames  ` +
        VISEMES.map((v) => `${v}:${seen.get(v) || 0}`).join(' '));
}

/* A picture of what the device will draw, so the translation can be checked with
 * eyes before anyone owns the panel. Not shipped; a build artefact for review. */
for (const st of STATES) {
    const d = DESIGN[st];
    const args: string[] = [BG_PNG];
    for (const layer of ['browL', 'eyeL', 'browR', 'eyeR', 'mouth'] as LayerId[]) {
        const part = layer === 'mouth' ? d.mouth : layer.startsWith('brow') ? d.brows : d.eyes;
        const s = sprites.find((x) => x.layer === layer && x.part === part);
        if (s?.png) args.push(s.png, '-geometry', `+${s.x}+${s.y}`, '-composite');
    }
    args.push(join(out, `preview-${st}.png`));
    execFileSync('magick', args);
}
console.log('previews: ' + STATES.map((s) => `preview-${s}.png`).join(', '));
console.log(`sprites: ${sprites.length}  tracks: ${trackRows.length}  keys: ${ki}`);
console.log(`face_assets.c: ${(cs.length / 1024).toFixed(0)} KB of C`);
for (const s of sprites) console.log(`  ${s.layer.padEnd(6)} ${s.part.padEnd(10)} ${s.w}x${s.h} at ${s.x},${s.y}`);
