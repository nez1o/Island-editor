# BC4 (one channel) encode / decode for small UI textures (the tab icons are 128x128 BC4 masks).
import struct

def palette(r0, r1):
    if r0 > r1: return [r0, r1] + [((7 - i) * r0 + i * r1 + 3) // 7 for i in range(1, 7)]
    return [r0, r1] + [((5 - i) * r0 + i * r1 + 2) // 5 for i in range(1, 5)] + [0, 255]

def decode(data, w, h):
    """linear BC4 blocks -> rows of 0-255"""
    bw = (w + 3) // 4
    img = [[0] * w for _ in range(h)]
    for by in range((h + 3) // 4):
        for bx in range(bw):
            o = (by * bw + bx) * 8
            r0, r1 = data[o], data[o + 1]
            bits = int.from_bytes(data[o + 2:o + 8], 'little')
            pal = palette(r0, r1)
            for i in range(16):
                x, y = bx * 4 + i % 4, by * 4 + i // 4
                if x < w and y < h: img[y][x] = pal[(bits >> (3 * i)) & 7]
    return img

def encode(img):
    """rows of 0-255 (w, h multiples of 4) -> linear BC4 blocks"""
    h, w = len(img), len(img[0])
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            px = [img[by + i // 4][bx + i % 4] for i in range(16)]
            hi, lo = max(px), min(px)
            if hi == lo:
                out += bytes([hi, lo]) + b'\0' * 6
                continue
            pal = palette(hi, lo)
            bits = 0
            for i, v in enumerate(px):
                k = min(range(8), key=lambda j: abs(pal[j] - v))
                bits |= k << (3 * i)
            out += bytes([hi, lo]) + bits.to_bytes(6, 'little')
    return bytes(out)
