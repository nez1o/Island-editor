# ASTC encoder / decoder for UI pictures: CEM 12 (RGBA direct LDR), one plane, 2-bit weights, the
# weight grid the size of the block (4x4 or 5x4). Generalised from phone_icon.py's 5x4 code.
import struct

W_UNQ = (0, 21, 43, 64)
CEM_RGBA = 12
MODES = {(4, 4): 0x42, (5, 4): 0xC2}   # block mode: W = B + 4, H = A + 2, weight range 0..3


def void_block(c):
    lo = 0x1FC | (0b11 << 10) | (0x1FFF << 12) | (0x1FFF << 25) | (0x1FFF << 38) | (0x1FFF << 51)
    return struct.pack('<Q', lo) + struct.pack('<4H', *[v * 257 for v in c])


def _interp(e0, e1, w):
    return tuple(((a * 257) * (64 - w) + (b * 257) * w + 32) >> 6 >> 8 for a, b in zip(e0, e1))


def encode_block(px, bw=4, bh=4):
    n = bw * bh
    if all(p[3] == 0 for p in px): return void_block((0, 0, 0, 0))
    if all(p == px[0] for p in px): return void_block(px[0])
    best = (-1, 0, 0)
    for i in range(n):
        for j in range(i + 1, n):
            dd = sum((a - b) ** 2 for a, b in zip(px[i], px[j]))
            if dd > best[0]: best = (dd, i, j)
    e0, e1 = list(px[best[1]]), list(px[best[2]])
    def weights():
        return [min(range(4), key=lambda k: sum((a - b) ** 2 for a, b in zip(_interp(e0, e1, W_UNQ[k]), p))) for p in px]
    for _ in range(2):
        t = [W_UNQ[k] / 64 for k in weights()]
        a11 = sum((1 - x) ** 2 for x in t); a12 = sum((1 - x) * x for x in t); a22 = sum(x * x for x in t)
        det = a11 * a22 - a12 * a12
        if abs(det) < 1e-9: break
        n0, n1 = [], []
        for ch in range(4):
            b1 = sum((1 - x) * p[ch] for x, p in zip(t, px)); b2 = sum(x * p[ch] for x, p in zip(t, px))
            n0.append(max(0, min(255, round((a22 * b1 - a12 * b2) / det))))
            n1.append(max(0, min(255, round((a11 * b2 - a12 * b1) / det))))
        e0, e1 = n0, n1
    ws = weights()
    if sum(e1[:3]) < sum(e0[:3]):
        e0, e1 = e1, e0
        ws = [3 - w for w in ws]
    bits = MODES[(bw, bh)] | (0 << 11) | (CEM_RGBA << 13)
    for i, v in enumerate([e0[0], e1[0], e0[1], e1[1], e0[2], e1[2], e0[3], e1[3]]):
        bits |= v << (17 + 8 * i)
    wstream = 0
    for i, w in enumerate(ws): wstream |= w << (2 * i)
    for i in range(2 * n):
        if (wstream >> i) & 1: bits |= 1 << (127 - i)
    return bits.to_bytes(16, 'little')


def encode(rows, bw=4, bh=4):
    """rows of RGBA tuples -> linear ASTC blocks"""
    h, w = len(rows), len(rows[0])
    out = bytearray()
    for by in range((h + bh - 1) // bh):
        for bx in range((w + bw - 1) // bw):
            px = [rows[min(by * bh + y, h - 1)][min(bx * bw + x, w - 1)] for y in range(bh) for x in range(bw)]
            out += encode_block(px, bw, bh)
    return bytes(out)
