# Pixel-art previews for the Island Editor's Bridges picker: one small side view per bridge
# material (log, suspension, stone, wooden, brick, iron, zen, red zen), each over a strip of
# river between two grassy banks. Drawn here by hand from simple shapes, so no game art is
# involved. Writes source/program/bridge_icons.inc (read by slumber_app.cpp) and optionally a
# zoomed PNG preview of all eight. No dependencies beyond the standard library.
#
# usage: python tools/bridge_icons.py source/program/bridge_icons.inc [preview.png]
import math
import struct
import sys
import zlib

W, H = 40, 26

# names in the order of the picker (must match k_BridgeMaterials in main.cpp)
NAMES = ['Log', 'Suspension', 'Stone', 'Wooden', 'Brick', 'Iron', 'Zen', 'Red zen']

OUT = (78, 56, 36)          # outline


def new():
    return [[None] * W for _ in range(H)]


def put(g, x, y, c):
    if 0 <= x < W and 0 <= y < H and c is not None:
        g[y][x] = c


def hline(g, x0, x1, y, c):
    for x in range(x0, x1 + 1): put(g, x, y, c)


def vline(g, x, y0, y1, c):
    for y in range(y0, y1 + 1): put(g, x, y, c)


def rect(g, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1): hline(g, x0, x1, y, c)


def line(g, x0, y0, x1, y1, c):
    dx, dy = abs(x1 - x0), -abs(y1 - y0)
    sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
    err = dx + dy
    while True:
        put(g, x0, y0, c)
        if x0 == x1 and y0 == y1: break
        e2 = 2 * err
        if e2 >= dy: err += dy; x0 += sx
        if e2 <= dx: err += dx; y0 += sy


def scenery(g):
    """River with ripples between two banks; the bridge goes on top."""
    water, light, deep = (118, 196, 226), (186, 232, 246), (92, 168, 204)
    rect(g, 0, 18, W - 1, H - 1, water)
    for y in range(18, H):
        if y >= 23: hline(g, 0, W - 1, y, deep)
    for x, y in ((9, 20), (10, 20), (22, 21), (23, 21), (24, 21), (15, 23), (16, 23), (30, 19), (31, 19)):
        put(g, x, y, light)
    grass, grass_l, dirt = (124, 196, 96), (160, 220, 120), (176, 128, 80)
    for x0, x1 in ((0, 4), (W - 5, W - 1)):
        rect(g, x0, 14, x1, H - 1, dirt)
        rect(g, x0, 13, x1, 15, grass)
        hline(g, x0, x1, 13, grass_l)


def hump(x, x0, x1, height):
    """Arch height above the ends at column x (0 at the ends, height in the middle)."""
    t = (x - x0) / (x1 - x0)
    return round(height * math.sin(math.pi * t))


def deck(g, x0, x1, y, rows, top, body, height=0):
    for x in range(x0, x1 + 1):
        yy = y - hump(x, x0, x1, height)
        put(g, x, yy, top)
        for r in range(1, rows): put(g, x, yy + r, body)
    return lambda x: y - hump(x, x0, x1, height)


def railing(g, x0, x1, top_y, post_c, rail_c, every, post_h=4, rail_rows=(0,)):
    """Posts every <every> columns and rails, following the deck's top top_y(x)."""
    for x in range(x0, x1 + 1):
        for r in rail_rows: put(g, x, top_y(x) - post_h + r, rail_c)
        if (x - x0) % every == 0 or x == x1:
            vline(g, x, top_y(x) - post_h, top_y(x) - 1, post_c)


def log_bridge():
    g = new(); scenery(g)
    bark, bark_d, end = (150, 100, 58), (112, 72, 40), (222, 182, 120)
    for y0 in (12, 14):   # two stacked logs
        hline(g, 3, 36, y0, bark); hline(g, 3, 36, y0 + 1, bark_d)
    for y in (12, 14):    # cut ends
        put(g, 3, y, end); put(g, 36, y, end)
    for x in (6, 33):     # stakes in the bank and a rope rail
        vline(g, x, 8, 16, bark_d)
    for x in range(6, 34):
        put(g, x, 8 + (1 if 10 < x < 29 else 0), (198, 168, 116))
    return g


def suspension():
    g = new(); scenery(g)
    post, rope, plank, gap = (120, 80, 46), (206, 176, 122), (186, 136, 80), (130, 90, 52)
    vline(g, 6, 3, 15, post); vline(g, 7, 3, 15, post)
    vline(g, 32, 3, 15, post); vline(g, 33, 3, 15, post)
    put(g, 6, 2, OUT); put(g, 7, 2, OUT); put(g, 32, 2, OUT); put(g, 33, 2, OUT)
    for x in range(8, 32):   # sagging main rope
        t = (x - 8) / 23
        cy = round(3 + 7 * (1 - (2 * t - 1) ** 2))
        put(g, x, cy, rope)
        if x % 3 == 0: vline(g, x, cy + 1, 11, rope)   # hangers
    for x in range(4, 36):
        dip = 1 if 12 < x < 27 else 0
        put(g, x, 12 + dip, plank if x % 3 else gap)
        put(g, x, 13 + dip, gap)
    return g


def stone_arch(g, body, mortar, top, rail):
    """A solid arched bridge: masonry from the deck down to a round opening over the water."""
    cx, cy, rad = 19.5, 24, 10
    for y in range(10, H):
        for x in range(3, 37):
            if (x - cx) ** 2 + (y - cy) ** 2 < rad * rad: continue   # the arch opening
            if y >= 18 and not (x < 8 or x > 31): continue
            c = body
            if y % 3 == 0 or (x + (y // 3) * 3) % 6 == 0: c = mortar   # block joints
            put(g, x, y, c)
    hline(g, 3, 36, 9, top); hline(g, 3, 36, 10, top)
    hline(g, 3, 36, 7, rail); hline(g, 3, 36, 8, body)
    for x in range(3, 37, 5): vline(g, x, 6, 8, rail)
    vline(g, 36, 6, 8, rail)
    return g


def stone():
    return stone_arch(scenery_g(), (160, 160, 152), (122, 122, 118), (198, 196, 186), (186, 184, 174))


def brick():
    return stone_arch(scenery_g(), (180, 86, 60), (222, 180, 146), (206, 120, 86), (150, 66, 46))


def scenery_g():
    g = new(); scenery(g); return g


def wooden():
    g = new(); scenery(g)
    top = deck(g, 3, 36, 12, 3, (226, 184, 124), (190, 142, 86), height=2)
    for x in (10, 29): vline(g, x, top(x) + 3, 19, (150, 104, 60))   # legs into the water
    railing(g, 3, 36, top, (176, 126, 72), (226, 184, 124), 4)
    return g


def iron():
    g = new(); scenery(g)
    steel, steel_d, steel_l = (92, 104, 122), (64, 74, 90), (150, 162, 180)
    rect(g, 3, 12, 36, 13, steel_d); hline(g, 3, 36, 12, steel)
    hline(g, 6, 33, 5, steel_l); hline(g, 6, 33, 6, steel)
    line(g, 3, 11, 6, 6, steel); line(g, 36, 11, 33, 6, steel)
    for k, x in enumerate(range(6, 34, 6)):   # Warren truss
        line(g, x, 6, x + 3, 11, steel); line(g, x + 3, 11, x + 6, 6, steel)
    vline(g, 6, 6, 11, steel_d); vline(g, 33, 6, 11, steel_d)
    for x in range(4, 36, 2): put(g, x, 14, steel_d)   # rivets / girder edge
    return g


def zen(rail, rail_cap, plank, plank_d, post):
    g = new(); scenery(g)
    top = deck(g, 3, 36, 13, 3, plank, plank_d, height=6)
    for x in (9, 30): vline(g, x, top(x) + 3, 19, post)
    railing(g, 3, 36, top, post, rail, 5, post_h=5, rail_rows=(0, 2))
    for x in range(3, 37, 5):   # round caps on the posts
        put(g, x, top(x) - 6, rail_cap)
    put(g, 36, top(36) - 6, rail_cap)
    return g


def zen_plain():
    return zen((116, 80, 52), (176, 140, 96), (152, 108, 68), (112, 76, 46), (96, 64, 40))


def zen_red():
    return zen((216, 62, 48), (240, 202, 92), (162, 86, 58), (122, 60, 40), (190, 46, 38))


ICONS = [log_bridge(), suspension(), stone(), wooden(), brick(), iron(), zen_plain(), zen_red()]


def tab_icon(size=128, ss=4):
    """The Bridges tab's icon for the house menu: a white arched-bridge silhouette (0-255 mask,
    like the game's IconCat* textures, which the layout tints)."""
    N = size * ss
    def deck_y(x):   # top of the deck at x (arch)
        t = (x - 0.06 * N) / (0.88 * N)
        return N * (0.70 - 0.20 * math.sin(math.pi * max(0.0, min(1.0, t))))
    def inside(x, y):
        if 0.06 * N <= x <= 0.94 * N:
            top = deck_y(x)
            if top <= y <= top + 0.115 * N: return True                       # deck
            rail = top - 0.30 * N
            if 0.12 * N <= x <= 0.88 * N and rail <= y <= rail + 0.075 * N: return True   # top rail
            for k in range(5):                                                # posts
                px = 0.125 * N + k * 0.175 * N
                if px <= x <= px + 0.065 * N and rail <= y <= top: return True
        for lx in (0.20, 0.71):                                               # legs into the water
            if lx * N <= x <= (lx + 0.07) * N and deck_y(x) <= y <= 0.94 * N: return True
        return False
    img = []
    for y in range(size):
        row = []
        for x in range(size):
            c = sum(inside(x * ss + i + 0.5, y * ss + j + 0.5) for i in range(ss) for j in range(ss))
            row.append(round(255 * c / (ss * ss)))
        img.append(row)
    return img


def incline_tab_icon(size=128, ss=4):
    """The Inclines tab's icon: a white four-step staircase going up to the right (0-255 mask)."""
    N = size * ss
    steps = 4
    def inside(x, y):
        # slope body: under the line from bottom-left to top-right
        x0, y0, x1, y1 = 0.08 * N, 0.90 * N, 0.92 * N, 0.18 * N
        if not (x0 <= x <= x1 and y <= y0): return False
        t = (x - x0) / (x1 - x0)
        # stairs: the top edge is a staircase following the slope
        k = min(steps - 1, int(t * steps))
        top = y0 - (y0 - y1) * (k + 1) / steps
        return y >= top
    img = []
    for y in range(size):
        row = []
        for x in range(size):
            c = sum(inside(x * ss + i + 0.5, y * ss + j + 0.5) for i in range(ss) for j in range(ss))
            row.append(round(255 * c / (ss * ss)))
        img.append(row)
    return img


def fit_mask(img, box=(10, 12, 117, 111)):
    """Scale / move a 0-255 mask so its shape fills <box> (x0, y0, x1, y1, inclusive) keeping its
    aspect, centred like the game's IconCat* tab icons (which span about x 10-117, y 12-111)."""
    n = len(img)
    ys = [y for y in range(n) if max(img[y]) > 64]
    xs = [x for x in range(n) if max(img[y][x] for y in range(n)) > 64]
    sx0, sx1, sy0, sy1 = min(xs), max(xs) + 1, min(ys), max(ys) + 1
    bx0, by0, bx1, by1 = box[0], box[1], box[2] + 1, box[3] + 1
    k = min((bx1 - bx0) / (sx1 - sx0), (by1 - by0) / (sy1 - sy0))
    w, h = (sx1 - sx0) * k, (sy1 - sy0) * k
    ox, oy = (bx0 + bx1 - w) / 2, (by0 + by1 - h) / 2
    out = [[0] * n for _ in range(n)]
    for y in range(n):
        for x in range(n):
            acc = 0
            for j in (0.25, 0.75):
                for i in (0.25, 0.75):
                    u = sx0 + (x + i - ox) / k; v = sy0 + (y + j - oy) / k
                    if sx0 <= u < sx1 and sy0 <= v < sy1: acc += img[int(v)][int(u)]
            out[y][x] = round(acc / 4)
    return out


def write_inc(path):
    pal = []
    for g in ICONS:
        for row in g:
            for c in row:
                if c is not None and c not in pal: pal.append(c)
    keys = '123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'
    assert len(pal) <= len(keys), len(pal)
    with open(path, 'w') as f:
        f.write('/* Bridge previews for the Island Editor\'s Bridges picker, drawn from simple shapes.\n'
                '   Generated by tools/bridge_icons.py - edit that, not this.\n'
                '   One char per pixel: "." transparent, else k_BridgePalette[index of the char in\n'
                '   k_BridgeKeys]. Order: ' + ', '.join(NAMES) + '. */\n')
        f.write('constexpr int BridgeIconW = %d, BridgeIconH = %d, BridgeIconCount = %d;\n' % (W, H, len(ICONS)))
        f.write('static const char k_BridgeKeys[] = "%s";\n' % keys[:len(pal)])
        f.write('static const uint8_t k_BridgePalette[][3] = {\n')
        for c in pal: f.write('    { %d, %d, %d },\n' % c)
        f.write('};\n')
        f.write('static const char* const k_BridgeIcon[][%d] = {\n' % H)
        for name, g in zip(NAMES, ICONS):
            f.write('    {   /* %s */\n' % name)
            for row in g:
                f.write('        "%s",\n' % ''.join('.' if c is None else keys[pal.index(c)] for c in row))
            f.write('    },\n')
        f.write('};\n')


def write_png(path, zoom=8):
    cols, gap = 4, 2
    cw, ch = (W + gap) * zoom, (H + gap) * zoom
    pw, ph = cw * cols, ch * ((len(ICONS) + cols - 1) // cols)
    bg = (250, 244, 226)
    px = [[bg] * pw for _ in range(ph)]
    for i, g in enumerate(ICONS):
        ox, oy = (i % cols) * cw + zoom, (i // cols) * ch + zoom
        for y in range(H):
            for x in range(W):
                c = g[y][x]
                if c is None: continue
                for yy in range(zoom):
                    row = px[oy + y * zoom + yy]
                    for xx in range(zoom): row[ox + x * zoom + xx] = c
    raw = b''.join(b'\0' + bytes(v for c in row for v in c) for row in px)
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', pw, ph, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    write_inc(sys.argv[1])
    if len(sys.argv) > 2: write_png(sys.argv[2])
