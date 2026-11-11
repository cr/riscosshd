#!/usr/bin/env python3
"""Generate the !SSHd icon sprites (RISC OS sprite file format, 16 colours).

Writes riscos/!SSHd/!Sprites22,ff9 (mode 20, square pixels, 34x34) and
riscos/!SSHd/!Sprites,ff9 (mode 12, 34x17) with sprites "!sshd" and
"sm!sshd".  Colours are indices into the standard Wimp 16-colour palette.
Run from the repository root; the output files are committed.
"""
import struct, os

# Wimp palette: 0 white, 1-6 greys (light to dark), 7 black, 8 dark blue,
# 9 yellow, 10 light green, 11 red, 12 cream, 13 dark green, 14 orange,
# 15 light blue.
PAL = {'.': None, 'K': 7, 'W': 0, 'g': 2, 'G': 4, 'B': 8, 'L': 15, 'Y': 9,
       'O': 14, 'D': 5}

# 34x34: a terminal window (dark blue screen, light blue title bar, white
# "> _" prompt) with a yellow padlock over its bottom-right corner.
BIG = """
..................................
..KKKKKKKKKKKKKKKKKKKKKKKKKKKKK...
.KggggggggggggggggggggggggggggK...
.KgLLLLLLLLLLLLLLLLLLLLLLLLLLgK...
.KgLLLLLLLLLLLLLLLLLLLLLLLLLLgK...
.KgggggggggggggggggggggggggggggK..
.KgKKKKKKKKKKKKKKKKKKKKKKKKKKgK...
.KgKBBBBBBBBBBBBBBBBBBBBBBBBKgK...
.KgKBBBBBBBBBBBBBBBBBBBBBBBBKgK...
.KgKBBWWBBBBBBBBBBBBBBBBBBBBKgK...
.KgKBBBWWBBBBBBBBBBBBBBBBBBBKgK...
.KgKBBBBWWBBBBBBBBBBBBBBBBBBKgK...
.KgKBBBWWBBBWWWWBBBBBBBBBBBBKgK...
.KgKBBWWBBBBWWWWBBBBBBBBBBBBKgK...
.KgKBBBBBBBBBBBBBBBBBBBBBBBBKgK...
.KgKBBBBBBBBBBBBBBBBBBBBBBBBKgK...
.KgKBBBBBBBBBBBBBBBBBKKKKKKBKgK...
.KgKBBBBBBBBBBBBBBBBKKYYYYKKKgK...
.KgKBBBBBBBBBBBBBBBKKYYKKYYKKK....
.KgKBBBBBBBBBBBBBBBKYYKKKKYYKK....
.KgKBBBBBBBBBBBBBBBKYYKBBKYYK.....
.KgKBBBBBBBBBBBBBBBKYYKBBKYYK.....
.KgKBBBBBBBBBBBBBBKKYYKKKKYYKK....
.KgKBBBBBBBBBBBBBKKKKKKKKKKKKKK...
.KgKBBBBBBBBBBBBBKYYYYYYYYYYYYK...
.KgKKKKKKKKKKKKKKKYYYYYYYYYYYYK...
.KggggggggggggggggKYYYYYKKYYYYYK..
..KKKKKKKKKKKKKKKKKYYYYYKKYYYYYK..
..................KYYYYYYKYYYYYK..
..................KYYYYYYKYYYYYK..
..................KYYYYYYYYYYYYK..
..................KOOOOOOOOOOOOK..
..................KKKKKKKKKKKKKK..
..................................
"""

def rows(art):
    r = [l for l in art.strip('\n').split('\n')]
    assert all(len(l) == 34 for l in r) and len(r) == 34, [len(l) for l in r]
    return r

def shrink(r):   # 34x34 -> 17x17 by sampling every other pixel
    return [l[::2] for l in r[::2]]

def halve_height(r):   # square pixels -> mode 12 rectangular pixels
    return r[::2]

def sprite(name, art, mode):
    h = len(art); w = len(art[0])
    words = (w * 4 + 31) // 32
    img = bytearray(); mask = bytearray()
    for line in art:
        pix = [PAL[c] for c in line]
        for part, src in ((img, [0 if p is None else p for p in pix]),
                          (mask, [0 if p is None else 0xF for p in pix])):
            row = bytearray(words * 4)
            for i, v in enumerate(src):
                row[i // 2] |= v << (4 * (i % 2))
            part += row
    hdr_len = 44
    total = hdr_len + len(img) + len(mask)
    hdr = struct.pack('<I12sIIIIIII', total, name.encode().ljust(12, b'\0'),
                      words - 1, h - 1, 0, (w * 4 - 1) % 32, hdr_len,
                      hdr_len + len(img), mode)
    return hdr + img + mask

def spritefile(sprites):
    body = b''.join(sprites)
    return struct.pack('<III', len(sprites), 16, 16 + len(body)) + body

big = rows(BIG)
here = os.path.dirname(os.path.abspath(__file__))
app = os.path.join(here, '..', 'riscos', '!SSHd')
with open(os.path.join(app, '!Sprites22,ff9'), 'wb') as f:
    f.write(spritefile([sprite('!sshd', big, 20), sprite('sm!sshd', shrink(big), 20)]))
with open(os.path.join(app, '!Sprites,ff9'), 'wb') as f:
    f.write(spritefile([sprite('!sshd', halve_height(big), 12),
                        sprite('sm!sshd', halve_height(shrink(big)), 12)]))
print('sprites written')
