# The "Gyroid's menu" phone app icon (formerly Slumber): drawn here, encoded as ASTC 5x4 sRGB, and written over the pixels of
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

# A pixel-art gyroid (the same sprite as the menu's header, source/program/gyroid_sprite.inc) with two
# little music notes - gyroids make sounds. Pixels are drawn as sharp squares; the whole figure gets the
# thick brown ink outline the game's phone icons have.
NOTE = [
    "...XXX",
    "...XXX",
    "...X..",
    "...X..",
    "...X..",
    ".XXX..",
    "XXXX..",
    ".XX...",
]

def _sprite_layer(size, rows, colour_of, scale, ox, oy):
    layer = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            c = colour_of(ch)
            if c: d.rectangle([ox + x * scale, oy + y * scale, ox + (x + 1) * scale - 1, oy + (y + 1) * scale - 1], fill=c)
    return layer

def _inked(layer, ink):
    """Layer with a solid brown outline of <ink> pixels around its opaque part."""
    a = layer.getchannel('A').point(lambda v: 255 if v else 0)
    grown = a.filter(ImageFilter.MaxFilter(ink * 2 + 1))
    out = Image.new('RGBA', layer.size, (0, 0, 0, 0))
    out.paste(BROWN, (0, 0), grown)
    out.alpha_composite(layer)
    return out

def draw_icon(size=180, hop=0, pose=0, blink=False, sway=0, wiggle=0):
    """hop: sprite pixels up (0-2), pose: arm pose (sprite frame), blink: eyes shut (pose 0 only),
    sway: sprite pixels right (-1..1), wiggle: notes shaken one way (+1) or the other (-1).
    All 0 is the romfs icon."""
    import os, sys
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, here)
    import gyroid_art
    rows, pal = gyroid_art.read_inc(os.path.join(here, '..', 'source', 'program', 'gyroid_sprite.inc'), 2 if blink else pose)
    keys = '123456789abcdef'
    def gyroid_colour(ch):
        return None if ch == '.' else tuple(pal[keys.index(ch)]) + (255,)
    w, h = len(rows[0]), len(rows)
    scale = min(132 // h, 124 // w)
    gw, gh = w * scale, h * scale
    gx, gy = (size - gw) // 2 - 14 + sway * scale, size - gh - 18 - hop * scale
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    img.alpha_composite(_inked(_sprite_layer(size, rows, gyroid_colour, scale, gx, gy), 5))
    # the notes bounce in counter-time: one rises while the other falls, swapping each arm flap
    note = lambda ch: CREAM if ch == 'X' else None
    rise = hop if pose == 0 else 2 - hop
    # while the gyroid blinks the notes wiggle: shaken sideways in opposite directions
    img.alpha_composite(_inked(_sprite_layer(size, NOTE, note, 5, 134 + wiggle * 4, 24 - rise * 5 - abs(wiggle) * 2), 4))
    img.alpha_composite(_inked(_sprite_layer(size, NOTE, note, 4, 150 - wiggle * 4, 82 - (2 - rise) * 5 + abs(wiggle) * 2), 4))
    return img

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

# ---------------------------------------------------------------- animation frames for the module
# The module finds the icon texture once the game loads it (by two of its ASTC blocks, which tiling keeps
# whole) and copies these frames in, so the phone's gyroid hops, sways and flaps like the menu's. Frame
# pose * 4 + step, then two blink frames (resting, eyes shut, the notes wiggled either way) that
# alternate every 50 ms during a blink; frame 0 is exactly the romfs icon.
ICON_STEPS, ICON_POSES = 4, 2
HOPS = (0, 1, 2, 1)       # per step: sprite pixels up
SWAYS = (0, -1, 0, 1)     # per step: sprite pixels sideways (mirrored for pose 1), like the menu

def write_frames(path):
    frames = []
    for pose in range(ICON_POSES):
        for step in range(ICON_STEPS):
            sway = SWAYS[step] if pose == 0 else -SWAYS[step]
            data, bw, bh = encode_image(draw_icon(180, HOPS[step], pose, sway=sway))
            frames.append(data)
            print('frame', len(frames) - 1, 'hop', HOPS[step], 'sway', sway, 'pose', pose)
    for wiggle in (-1, 1):   # last two frames: resting, eyes shut, notes wiggling
        frames.append(encode_image(draw_icon(180, 0, 0, blink=True, wiggle=wiggle))[0])
        print('frame', len(frames) - 1, 'blink, wiggle', wiggle)
    blocks = [frames[0][i:i + 16] for i in range(0, len(frames[0]), 16)]
    # signature: the two most "busy" blocks that occur once in the icon and are not solid (void) blocks
    def busy(b): return len(set(b))
    cands = [b for b in blocks if blocks.count(b) == 1 and (int.from_bytes(b[:2], 'little') & 0x1ff) != 0x1fc]
    sig = sorted(cands, key=busy, reverse=True)[:2]
    assert len(sig) == 2
    out = ['/* Generated by tools/phone_icon.py --frames - do not edit.',
           '   The phone icon\'s animation frames (ASTC 5x4, 180x180, linear block order, hex) and two of',
           '   frame 0\'s blocks the module uses to recognise the icon texture once the game loads it. */',
           'constexpr int IconFrameCount = %d, IconSteps = %d, IconSize = 180, IconFrameBytes = %d;'
           % (len(frames), ICON_STEPS, len(frames[0])),
           'static const char k_IconSig[2][33] = { "%s", "%s" };' % (sig[0].hex(), sig[1].hex()),
           'static const char k_IconFrames[] =']
    data = b''.join(frames).hex()
    out += ['    "%s"' % data[i:i + 120] for i in range(0, len(data), 120)]
    out += [';', '']
    open(path, 'w').write('\n'.join(out))
    print('wrote', path)

if __name__ == '__main__':
    import sys
    if len(sys.argv) > 2 and sys.argv[1] == '--frames':
        write_frames(sys.argv[2])
        sys.exit(0)
    out = sys.argv[1] if len(sys.argv) > 1 else 'gyroid_icon.png'
    icon = draw_icon()
    icon.save(out)
    data, bw, bh = encode_image(icon)
    dec = decode_image(data, 180, 180)
    dec.save(out.replace('.png', '_astc.png'))
    print('encoded', len(data), 'bytes,', bw, 'x', bh, 'blocks')
