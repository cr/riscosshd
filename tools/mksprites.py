#!/usr/bin/env python3
"""Generate the !SSHd icon sprites (RISC OS sprite file format, 16 colours).

Writes riscos/!SSHd/!Sprites22,ff9 (mode 20, square pixels, 34x34) and
riscos/!SSHd/!Sprites,ff9 (mode 12, 34x17) with sprites "!sshd",
"sm!sshd" (17x17, drawn separately, as sampling the large one loses the
prompt and the keyhole) and "sshdfail" (greyed, shown on the icon bar
when the server failed to start).  Colours are indices into the standard
Wimp 16-colour palette.
The output files are committed; run it from anywhere.
"""
import struct, os

# Wimp palette: 0 white, 1-6 greys (light to dark), 7 black, 8 dark blue,
# 9 yellow, 10 light green, 11 red, 12 cream, 13 dark green, 14 orange,
# 15 light blue.
PAL = {'.': None, 'K': 7, 'W': 0, 'g': 2, 'd': 4, 'B': 8, 'C': 12, 'Y': 9, 'O': 14}

# 34x34: a RISC OS window (cream title bar, dark blue screen, white "* _"
# command prompt) with a yellow padlock over its bottom-right corner.
# Drawn with rectangles so that every edge lands on the intended column.
W = H = 34

def blank():
    return [['.'] * W for _ in range(H)]

def rect(g, x0, y0, x1, y1, c):       # inclusive corners, y down
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            g[y][x] = c

def frame(g, x0, y0, x1, y1, c):      # 1-pixel outline
    rect(g, x0, y0, x1, y0, c); rect(g, x0, y1, x1, y1, c)
    rect(g, x0, y0, x0, y1, c); rect(g, x1, y0, x1, y1, c)

def draw_icon():
    g = blank()
    # window: black outline, grey frame, cream title bar, black-edged screen
    rect(g, 2, 2, 29, 25, 'g')
    frame(g, 1, 1, 30, 26, 'K')
    for (x, y) in ((1, 1), (30, 1), (1, 26), (30, 26)):   # rounded corners
        g[y][x] = '.'
    rect(g, 3, 3, 28, 5, 'C')
    rect(g, 4, 8, 27, 23, 'B')
    frame(g, 3, 7, 28, 24, 'K')
    # prompt "* _": a five-pixel star, then the cursor
    for (x, y) in ((8, 10), (8, 11), (8, 13), (8, 14), (6, 12), (7, 12), (9, 12), (10, 12),
                   (6, 10), (10, 10), (6, 14), (10, 14), (8, 12)):
        g[y][x] = 'W'
    rect(g, 13, 13, 16, 14, 'W')
    # padlock: shackle (black outline, yellow, hollow), then the body
    rect(g, 20, 16, 29, 22, 'K')
    g[16][20] = g[16][29] = 'B'
    rect(g, 21, 17, 28, 22, 'Y')
    rect(g, 23, 19, 26, 22, 'B')
    rect(g, 18, 22, 31, 32, 'K')
    rect(g, 19, 23, 30, 31, 'Y')
    rect(g, 19, 31, 30, 31, 'O')
    rect(g, 24, 25, 25, 28, 'K')      # keyhole
    return [''.join(row) for row in g]

def draw_small():
    """17x17: the same picture with each part placed by hand."""
    g = [['.'] * 17 for _ in range(17)]
    rect(g, 1, 1, 14, 12, 'g')
    frame(g, 0, 0, 15, 13, 'K')
    rect(g, 1, 1, 14, 2, 'C')
    rect(g, 2, 4, 13, 11, 'B')
    for (x, y) in ((4, 5), (3, 6), (4, 6), (5, 6), (4, 7)):   # small star
        g[y][x] = 'W'
    rect(g, 6, 7, 7, 7, 'W')
    rect(g, 10, 8, 14, 10, 'K')           # padlock shackle
    rect(g, 11, 9, 13, 10, 'Y')
    g[9][12] = 'B'
    rect(g, 9, 11, 16, 16, 'K')           # padlock body
    rect(g, 10, 12, 15, 15, 'Y')
    g[13][12] = g[14][12] = 'K'           # keyhole
    return [''.join(row) for row in g]

def greyed(r):   # the failed-start version: every colour to grey
    return [l.translate(str.maketrans('BYOCW', 'dggg' + 'g')) for l in r]

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

big = draw_icon()
small = draw_small()
assert len(big) == 34 and all(len(l) == 34 for l in big)
assert len(small) == 17 and all(len(l) == 17 for l in small)
here = os.path.dirname(os.path.abspath(__file__))
app = os.path.join(here, '..', 'riscos', '!SSHd')
with open(os.path.join(app, '!Sprites22,ff9'), 'wb') as f:
    f.write(spritefile([sprite('!sshd', big, 20), sprite('sm!sshd', small, 20),
                        sprite('sshdfail', greyed(big), 20)]))
with open(os.path.join(app, '!Sprites,ff9'), 'wb') as f:
    f.write(spritefile([sprite('!sshd', halve_height(big), 12),
                        sprite('sm!sshd', halve_height(small), 12),
                        sprite('sshdfail', halve_height(greyed(big)), 12)]))
print('sprites written')
