# The Slumber phone app icon: drawn here, encoded as ASTC 5x4 sRGB, and written over the pixels of
# DeviceIconBook^x (180x180) in LMenuDeviceBtn's texture archive. That texture is only shown by frame 4
# of the button's "Type" animation, which no app uses any more (Critterpedia moved to frame 19), so the
# module points the Slumber app at frame 4.
#
# Encoder: every 5x4 block is either a solid-colour "void extent" block or a single-partition LDR RGBA
# block (CEM 12) with a 5x4 grid of 2-bit weights and 8-bit endpoints - block mode 0xC2, which the
# game's own icons use too. A matching decoder renders the result for checking.
import struct
from PIL import Image, ImageDraw, ImageFilter

BROWN = (76, 60, 51, 255)       # the outline/ink colour of the game's phone icons (#4C3C33)
CREAM = (247, 243, 225, 255)    # #F7F3E1
YELLOW = (247, 211, 92, 255)
SOFT = (255, 238, 170, 255)
PINK = (240, 150, 160, 255)

def draw_icon(size=180):
    s = 4                                    # supersample, then downscale for smooth edges
    W = size * s
    img = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    def P(*xy): return [v * s for v in xy]
    ink = 7 * s

    # Cloud (bottom): overlapping puffs, brown outline drawn by a bigger brown silhouette underneath.
    puffs = [(34, 128, 30), (66, 112, 34), (104, 108, 36), (140, 124, 28), (88, 140, 30), (52, 146, 22), (124, 146, 22)]
    for cx, cy, r in puffs:
        d.ellipse(P(cx - r, cy - r, cx + r, cy + r), fill=BROWN)
    d.rectangle(P(30, 128, 150, 162), fill=BROWN)
    for cx, cy, r in puffs:
        rr = r - 7
        d.ellipse(P(cx - rr, cy - rr, cx + rr, cy + rr), fill=CREAM)
    d.rectangle(P(37, 128, 143, 155), fill=CREAM)

    # Crescent moon resting on the cloud, with a sleeping face.
    mx, my, mr = 74, 72, 44
    moon = Image.new('L', (W, W), 0)
    md = ImageDraw.Draw(moon)
    md.ellipse(P(mx - mr, my - mr, mx + mr, my + mr), fill=255)
    md.ellipse(P(mx - mr + 30, my - mr - 14, mx + mr + 26, my + mr - 18), fill=0)
    outline = moon.filter(ImageFilter.MaxFilter(ink * 2 + 1))
    img.paste(BROWN, (0, 0), outline)
    img.paste(YELLOW, (0, 0), moon)
    # closed eye and a little blush
    d.arc(P(52, 70, 70, 84), start=20, end=160, fill=BROWN, width=5 * s)
    d.ellipse(P(44, 86, 54, 94), fill=PINK)

    # "Z z" floating up to the right.
    def zed(x, y, h, w):
        pts = [(x, y), (x + w, y), (x, y + h), (x + w, y + h)]
        d.line([tuple(v * s for v in p) for p in pts], fill=BROWN, width=8 * s, joint='curve')
    zed(118, 20, 26, 26)
    zed(146, 54, 16, 16)

    # Two little stars.
    def star(cx, cy, r):
        import math
        pts = []
        for i in range(10):
            a = -math.pi / 2 + i * math.pi / 5
            rr = r if i % 2 == 0 else r * 0.45
            pts.append(((cx + rr * math.cos(a)) * s, (cy + rr * math.sin(a)) * s))
        d.polygon(pts, fill=SOFT, outline=BROWN, width=3 * s)
    star(128, 84, 11)
    star(26, 34, 9)

    return img.resize((size, size), Image.LANCZOS)

# ---------------------------------------------------------------- ASTC 5x4 (sRGB) encoder / decoder

BW, BH = 5, 4
MODE_C2 = 0xC2          # 2D, weight grid 5x4, weight range 0..3, single plane
CEM_RGBA = 12
W_UNQ = (0, 21, 43, 64)

def void_block(c):
    lo = 0x1FC | (0b11 << 10) | (0x1FFF << 12) | (0x1FFF << 25) | (0x1FFF << 38) | (0x1FFF << 51)
    return struct.pack('<Q', lo) + struct.pack('<4H', *[v * 257 for v in c])

def _interp(e0, e1, w):
    return tuple(((a * 257) * (64 - w) + (b * 257) * w + 32) >> 6 >> 8 for a, b in zip(e0, e1))

def encode_block(px):
    """px: 20 RGBA tuples (row-major 5x4)."""
    if all(p[3] == 0 for p in px):
        return void_block((0, 0, 0, 0))
    if all(p == px[0] for p in px):
        return void_block(px[0])
    # endpoints: the two most distant pixels, then a couple of least-squares refinements
    best = (-1, 0, 0)
    for i in range(20):
        for j in range(i + 1, 20):
            dd = sum((a - b) ** 2 for a, b in zip(px[i], px[j]))
            if dd > best[0]: best = (dd, i, j)
    e0, e1 = list(px[best[1]]), list(px[best[2]])
    for _ in range(3):
        ws = []
        for p in px:
            ws.append(min(range(4), key=lambda k: sum((a - b) ** 2 for a, b in zip(_interp(e0, e1, W_UNQ[k]), p))))
        # least squares for e0, e1 given weights t = w/64
        t = [W_UNQ[k] / 64 for k in ws]
        a11 = sum((1 - x) ** 2 for x in t); a12 = sum((1 - x) * x for x in t); a22 = sum(x * x for x in t)
        det = a11 * a22 - a12 * a12
        if abs(det) < 1e-9: break
        n0, n1 = [], []
        for ch in range(4):
            b1 = sum((1 - x) * p[ch] for x, p in zip(t, px)); b2 = sum(x * p[ch] for x, p in zip(t, px))
            n0.append(max(0, min(255, round((a22 * b1 - a12 * b2) / det))))
            n1.append(max(0, min(255, round((a11 * b2 - a12 * b1) / det))))
        e0, e1 = n0, n1
    ws = [min(range(4), key=lambda k: sum((a - b) ** 2 for a, b in zip(_interp(e0, e1, W_UNQ[k]), p))) for p in px]
    # CEM 12 swaps (and blue-contracts) when sum(e1.rgb) < sum(e0.rgb): keep e1 the brighter one
    if sum(e1[:3]) < sum(e0[:3]):
        e0, e1 = e1, e0
        ws = [3 - w for w in ws]
    bits = MODE_C2 | (0 << 11) | (CEM_RGBA << 13)
    vals = [e0[0], e1[0], e0[1], e1[1], e0[2], e1[2], e0[3], e1[3]]
    for i, v in enumerate(vals):
        bits |= v << (17 + 8 * i)
    wstream = 0
    for i, w in enumerate(ws):
        wstream |= w << (2 * i)
    for i in range(40):                       # weights are stored bit-reversed from the top
        if (wstream >> i) & 1:
            bits |= 1 << (127 - i)
    return bits.to_bytes(16, 'little')

def decode_block(b):
    v = int.from_bytes(b, 'little')
    if v & 0x1FF == 0x1FC:
        c = struct.unpack_from('<4H', b, 8)
        return [tuple(x >> 8 for x in c)] * 20
    assert v & 0x7FF == MODE_C2 and (v >> 13) & 0xF == CEM_RGBA, hex(v & 0x1ffff)
    vals = [(v >> (17 + 8 * i)) & 0xFF for i in range(8)]
    e0 = (vals[0], vals[2], vals[4], vals[6]); e1 = (vals[1], vals[3], vals[5], vals[7])
    ws = []
    for i in range(20):
        w = ((v >> (127 - 2 * i)) & 1) | (((v >> (126 - 2 * i)) & 1) << 1)
        ws.append(w)
    return [_interp(e0, e1, W_UNQ[w]) for w in ws]

def encode_image(img):
    img = img.convert('RGBA')
    w, h = img.size
    bw, bh = (w + BW - 1) // BW, (h + BH - 1) // BH
    pix = img.load()
    out = bytearray()
    for by in range(bh):
        for bx in range(bw):
            px = []
            for y in range(BH):
                for x in range(BW):
                    X, Y = min(bx * BW + x, w - 1), min(by * BH + y, h - 1)
                    px.append(pix[X, Y])
            out += encode_block(px)
    return bytes(out), bw, bh

def decode_image(data, w, h):
    bw, bh = (w + BW - 1) // BW, (h + BH - 1) // BH
    img = Image.new('RGBA', (bw * BW, bh * BH))
    for by in range(bh):
        for bx in range(bw):
            px = decode_block(data[(by * bw + bx) * 16:(by * bw + bx) * 16 + 16])
            for i, p in enumerate(px):
                img.putpixel((bx * BW + i % BW, by * BH + i // BW), p)
    return img.crop((0, 0, w, h))

if __name__ == '__main__':
    import sys
    out = sys.argv[1] if len(sys.argv) > 1 else 'slumber_icon.png'
    icon = draw_icon()
    icon.save(out)
    data, bw, bh = encode_image(icon)
    dec = decode_image(data, 180, 180)
    dec.save(out.replace('.png', '_astc.png'))
    print('encoded', len(data), 'bytes,', bw, 'x', bh, 'blocks')
