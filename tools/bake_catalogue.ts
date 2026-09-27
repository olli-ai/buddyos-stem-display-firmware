/*
 * Bake every character into the `faces` partition.
 *
 * The app image can hold one mascot's artwork. There are six, and a child who picks
 * Leaf on the website should get Leaf on their kit — so the artwork moves off the
 * app image and into the partition ADR 0009 reserves for exactly this, where it can
 * also be replaced without a firmware release.
 *
 * What goes in: each base's **background** (its own render, with the painted eyes and
 * mouth patched out, RGB565A8 so the body can be tinted) and its **own drawn-in face**
 * — the `artwork` eye and mouth parts, which are bitmaps an artist drew rather than
 * shapes anything can compute. The catalogue of *drawn* parts stays in the app: those
 * are the same curves on every base.
 *
 *   cd ../buddyos-stem-fe && npx esbuild ../buddyos-stem-display-firmware/tools/bake_catalogue.ts \
 *     --bundle --platform=node --format=cjs --outfile=/tmp/cat.cjs \
 *     --loader:.webp=dataurl --loader:.png=dataurl \
 *   && node /tmp/cat.cjs ../buddyos-stem-display-firmware/build/face_catalogue.bin
 */
import { execFileSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { LOOKS } from '../../buddyos-stem-fe/src/data/looks';

const SIZE = 240;                       // the GC9A01 is 240x240
const MAGIC = 'BUDDYCAT';
const VERSION = 1;
const tmp = mkdtempSync(join(tmpdir(), 'cat-'));

/** A data: URL from esbuild back into a file rsvg and ImageMagick can read. */
function asFile(dataUrl: string, name: string): string {
    const comma = dataUrl.indexOf(',');
    const body = dataUrl.slice(comma + 1);
    const base64 = dataUrl.slice(0, comma).includes(';base64');
    const p = join(tmp, name);
    writeFileSync(p, Buffer.from(decodeURIComponent(body), base64 ? 'base64' : 'utf8'));
    return p;
}

/* Kept for the record, unused: see the note in background(). */
/**
 * Where this character's *head* is, in face units.
 *
 * Sprout, Pip and Leaf are heads that fill the frame. Robot, Cardboard and Board are
 * whole objects — a robot with arms, a box on wheels, a dev board — whose faces
 * occupy a corner. Shown unchanged on a 240 px round panel, the body wins and the
 * eyes end up a few pixels across, which is the wrong bargain for a display whose
 * entire job is a face.
 *
 * So each character is framed on its head, using the anchors the front end already
 * keeps for placing drawn parts. No new artwork, and the characters that already
 * fill the frame barely move.
 */
function headBox(base: any): { x: number; y: number; s: number } {
    const a = base.anchors;
    const hw = a.halfWidth * 1.12;
    const chin = Math.max(a.cheeks[0][1], a.cheeks[1][1]) + a.halfWidth * 0.45;
    const cx = a.top[0];
    const cy = (a.top[1] + chin) / 2;
    /* Square, because the panel is: a non-square crop would squash the face. */
    const half = Math.max(hw, (chin - a.top[1]) / 2 + hw * 0.08);
    return { x: cx - half, y: cy - half, s: 2 * half };
}

const svgDoc = (inner: string) =>
    `<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 100 100" width="${SIZE}" height="${SIZE}">${inner}</svg>`;

/** The background, transparent outside the silhouette so the body can be recoloured. */
function background(base: any, n: string): Buffer {
    const src = asFile(base.faces.happy, `${n}.webp`);
    const paint = base.paint;
    const fill = paint.fill || base.swatch;
    const k = paint.patchScale || 1.6;
    const grad = `<defs><radialGradient id="g" cx="0.5" cy="0.5" r="0.5">
        <stop offset="0" stop-color="${fill}" stop-opacity="1"/>
        <stop offset="0.88" stop-color="${fill}" stop-opacity="1"/>
        <stop offset="1" stop-color="${fill}" stop-opacity="0"/></radialGradient></defs>`;
    const ells = [
        ...paint.eyes.map(([cx, cy]: number[]) => ({ cx, cy, rx: paint.eyeR[0] * k, ry: paint.eyeR[1] * k })),
        { cx: paint.mouth[0], cy: paint.mouth[1], rx: paint.mouthR[0] * k, ry: paint.mouthR[1] * k },
    ].map((e) => `<ellipse cx="${e.cx}" cy="${e.cy}" rx="${e.rx}" ry="${e.ry}" fill="url(#g)"/>`).join('');
    const pf = join(tmp, `${n}-patch.png`), bg = join(tmp, `${n}-bg.png`);
    writeFileSync(join(tmp, `${n}.svg`), svgDoc(grad + ells));
    execFileSync('rsvg-convert', ['-w', String(SIZE), '-h', String(SIZE), '-o', pf, join(tmp, `${n}.svg`)]);
    /* No cropping. Framing each character on its head was tried and reverted: it
     * improved the ones that were already heads and broke the ones it was meant to
     * fix. Robot and Board wear their faces as a screen and a sticker, sized nothing
     * like the `paint` ellipses, so the patch leaves the original features showing
     * and the artwork parts land on top of them — two faces, one head. Cropping
     * magnified that rather than causing it. Those characters need head-first
     * artwork, which is an art decision and not a generator one. */
    execFileSync('magick', ['-size', `${SIZE}x${SIZE}`, 'xc:none', '(', src, '-resize', `${SIZE}x${SIZE}`, ')',
        '-gravity', 'center', '-composite', pf, '-gravity', 'center', '-composite', bg]);
    const rgba = execFileSync('magick', [bg, '-depth', '8', 'RGBA:-'], { maxBuffer: 1 << 28 });
    /* RGB565A8 the way LVGL stores it: the colour plane whole, then the alpha plane
     * whole. Interleaving three bytes per pixel renders as noise. */
    const out = Buffer.alloc(SIZE * SIZE * 3);
    const a0 = SIZE * SIZE * 2;
    for (let i = 0, p = 0; i < rgba.length; i += 4, p++) {
        const v = ((rgba[i] & 0xf8) << 8) | ((rgba[i + 1] & 0xfc) << 3) | (rgba[i + 2] >> 3);
        out[p * 2] = v & 0xff; out[p * 2 + 1] = v >> 8;
        out[a0 + p] = rgba[i + 3];
    }
    return out;
}

/** One of the base's own drawn-in features, placed where its artist put it. */
function artPart(box: any, n: string): { px: Buffer; w: number; h: number; x: number; y: number } {
    const src = asFile(box.href, `${n}.png`);
    const w = Math.round((box.w / 100) * SIZE), h = Math.round((box.h / 100) * SIZE);
    const x = Math.round((box.x / 100) * SIZE), y = Math.round((box.y / 100) * SIZE);
    const out = join(tmp, `${n}-fit.png`);
    execFileSync('magick', [src, '-resize', `${w}x${h}!`, '-depth', '8', out]);
    const rgba = execFileSync('magick', [out, '-depth', '8', 'RGBA:-'], { maxBuffer: 1 << 28 });
    const px = Buffer.alloc(rgba.length);
    for (let i = 0; i < rgba.length; i += 4) {      // LVGL ARGB8888 is BGRA in memory
        px[i] = rgba[i + 2]; px[i + 1] = rgba[i + 1]; px[i + 2] = rgba[i]; px[i + 3] = rgba[i + 3];
    }
    return { px, w, h, x, y };
}

const outPath = process.argv[2] || 'face_catalogue.bin';

/* The characters this panel carries, decided 2026-09-27 after seeing all six on it.
 *
 * These three are *heads*: they fill the frame, and their features sit where `paint`
 * says, so the baker can erase the original face and a child's chosen eyes land where
 * the old ones were. Cardboard, Robot and Board are objects that happen to have a
 * face — a box on wheels, a robot with arms, a dev board — worn as a printed sticker
 * or a lit screen, nothing like those ellipses. On a 240 px round panel their bodies
 * win the circle and their eyes come out a few pixels across, and the patch misses
 * their real features so drawn parts land beside them rather than over them.
 *
 * They stay on the website, where a kit card is the right place for a picture of the
 * kit. A kit whose design names one shows the built-in character and says so. Give
 * them head-first artwork and they belong here too — nothing else needs to change. */
const ON_DEVICE = ['sprout', 'pip', 'leaf'];
const bases = Object.values(LOOKS).filter((b: any) =>
    b.paint && b.faces?.happy && ON_DEVICE.includes(b.id)) as any[];

/* Layout: header, then one fixed-size record per base, then everything's pixels.
 * Offsets are from the start of the file so the device can mmap the whole partition
 * and point LVGL straight at it — no copying, no RAM. */
const HDR = 16, REC = 96;
const recs: Buffer[] = [];
const blobs: Buffer[] = [];
let off = HDR + REC * bases.length;

const put = (b: Buffer) => { const at = off; blobs.push(b); off += b.length; 
    /* 4-byte alignment: LVGL reads 16- and 32-bit pixels straight out of this. */
    const pad = (4 - (off % 4)) % 4; if (pad) { blobs.push(Buffer.alloc(pad)); off += pad; }
    return at; };

for (const base of bases) {
    const bg = background(base, base.id);
    const bgOff = put(bg);
    const rec = Buffer.alloc(REC);
    rec.write(base.id, 0, 16, 'utf8');
    rec.writeUInt16LE(SIZE, 16); rec.writeUInt16LE(SIZE, 18);
    rec.writeUInt32LE(bgOff, 20); rec.writeUInt32LE(bg.length, 24);
    const art = base.paint.art;
    ['eyeL', 'eyeR', 'mouth'].forEach((k, i) => {
        const at = 28 + i * 20;
        if (!art?.[k]) return;
        const p = artPart(art[k], `${base.id}-${k}`);
        const o = put(p.px);
        rec.writeUInt16LE(p.w, at); rec.writeUInt16LE(p.h, at + 2);
        rec.writeInt16LE(p.x, at + 4); rec.writeInt16LE(p.y, at + 6);
        rec.writeUInt32LE(o, at + 8); rec.writeUInt32LE(p.px.length, at + 12);
    });
    recs.push(rec);
    console.log(`  ${base.id.padEnd(10)} bg ${(bg.length / 1024).toFixed(0)} KB` +
        (art ? `  + ${['eyeL', 'eyeR', 'mouth'].filter((k) => art[k]).length} artwork parts` : ''));
}

const hdr = Buffer.alloc(HDR);
hdr.write(MAGIC, 0, 8, 'ascii');
hdr.writeUInt16LE(VERSION, 8);
hdr.writeUInt16LE(bases.length, 10);
hdr.writeUInt32LE(off, 12);
const file = Buffer.concat([hdr, ...recs, ...blobs]);
writeFileSync(outPath, file);
console.log(`${outPath}: ${(file.length / 1024).toFixed(0)} KB, ${bases.length} characters` +
    `  (partition is 1536 KB, ${(100 * file.length / (1536 * 1024)).toFixed(0)}% used)`);
