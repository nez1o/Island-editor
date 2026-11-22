# Minimal PNG reader / writer (8-bit RGB / RGBA / grey, non-interlaced) for the build tools.
import struct, zlib


def read(path):
    """-> (w, h, rows of RGBA tuples)"""
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, plte = 8, b'', None
    while pos < len(d):
        n = struct.unpack('>I', d[pos:pos + 4])[0]; t = d[pos + 4:pos + 8]; body = d[pos + 8:pos + 8 + n]
        if t == b'IHDR': w, h, depth, ctype, _c, _f, inter = struct.unpack('>IIBBBBB', body)
        elif t == b'IDAT': idat += body
        elif t == b'PLTE': plte = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        pos += 12 + n
    assert depth == 8 and inter == 0, (depth, inter)
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * bpp
    prev = bytearray(stride)
    rows = []
    o = 0
    for _ in range(h):
        f = raw[o]; line = bytearray(raw[o + 1:o + 1 + stride]); o += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if ctype == 6: rows.append([tuple(line[i:i + 4]) for i in range(0, stride, 4)])
        elif ctype == 2: rows.append([tuple(line[i:i + 3]) + (255,) for i in range(0, stride, 3)])
        elif ctype == 0: rows.append([(v, v, v, 255) for v in line])
        elif ctype == 4: rows.append([(line[i], line[i], line[i], line[i + 1]) for i in range(0, stride, 2)])
        else: rows.append([plte[v] + (255,) for v in line])
    return w, h, rows


def write(path, rows):
    h, w = len(rows), len(rows[0])
    raw = b''.join(b'\0' + bytes(v for p in r for v in p) for r in rows)
    def ch(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))
