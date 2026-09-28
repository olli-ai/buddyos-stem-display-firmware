#!/usr/bin/env python3
"""Turn a kit's voice tap back into audio.

With CONFIG_BUDDY_VOICE_TAP on, the kit prints every Opus packet it sends omni as
"TAP <frame> <base64>". Save the monitor output to a file, then:

    tools/voice_tap.py capture.log out.opus     # Ogg Opus, 48 kHz mono, 20 ms packets

Gaps in the frame counter (the child's turn ended, or the reply was playing) are kept
as gaps: each run of consecutive frames is written in order, so what you hear is what
omni heard. ffmpeg -i out.opus out.wav gives a WAV.
"""
import base64, re, struct, sys

def _crc(b):
    c = 0
    for x in b:
        c ^= x << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
    return c

def ogg_write(path, packets):
    serial, seq, out = 0x5eed, 0, bytearray()
    def page(payload, granule, flags):
        nonlocal seq
        lace = bytes([255] * (len(payload) // 255) + [len(payload) % 255])
        h = (b'OggS' + bytes([0, flags]) + struct.pack('<qII', granule, serial, seq)
             + b'\0\0\0\0' + bytes([len(lace)]) + lace)
        pg = bytearray(h + payload)
        pg[22:26] = struct.pack('<I', _crc(pg))
        out.extend(pg); seq += 1
    page(b'OpusHead' + bytes([1, 1]) + struct.pack('<HIhB', 312, 48000, 0, 0), 0, 2)
    page(b'OpusTags' + struct.pack('<I', 3) + b'kit' + struct.pack('<I', 0), 0, 0)
    g = 0
    for k, p in enumerate(packets):
        g += 960
        page(p, g, 4 if k == len(packets) - 1 else 0)
    open(path, 'wb').write(out)

def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    tap = re.compile(r'TAP (\d+) ([A-Za-z0-9+/=]+)')
    packets, runs, last = [], 0, None
    for line in open(sys.argv[1], errors='replace'):
        m = tap.search(line)
        if not m:
            continue
        frame = int(m.group(1))
        try:
            packets.append(base64.b64decode(m.group(2), validate=True))
        except ValueError:
            continue  # a line the console mangled
        if last is None or frame != last + 1:
            runs += 1
        last = frame
    if not packets:
        sys.exit('no TAP lines found')
    ogg_write(sys.argv[2], packets)
    print(f'{len(packets)} packets ({len(packets) * 0.02:.1f} s) in {runs} run(s) -> {sys.argv[2]}')

if __name__ == '__main__':
    main()
