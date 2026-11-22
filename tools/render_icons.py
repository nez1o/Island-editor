# Renders the game's own bridge and incline models into pictures for the Bridges / Inclines tabs of
# the house menu (see bridges_tab.py). Runs at build time on the player's own RomFS dump; nothing
# from the game goes into the repository. Pure Python (no numpy): a small BFRES reader (models,
# shapes, vertex / index buffers, the albedo texture of each material), a BC1 / BC3 decoder for the
# embedded BNTX, and a z-buffered, textured, Lambert-lit rasteriser.
#
# usage: python tools/render_icons.py <RomFS dir> <out dir for PNG previews> [model ...]
import math, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
FLIPV = os.environ.get('FLIPV', '0') == '1'
CLIPY = float(os.environ.get('CLIPY', '-1e9'))
sys.path.insert(0, HERE)
import swz
from bntx import textures as bntx_textures


# ---------------------------------------------------------------- BFRES (Switch, v0.9)
def _str(d, p):
    n = struct.unpack_from('<H', d, p)[0]
    return d[p + 2:p + 2 + n].decode()


def _q(d, p): return struct.unpack_from('<Q', d, p)[0]


def read_bfres(d):
    """-> dict(models={name: [shape dict(pos, uv, idx, material)]}, textures={name: (w, h, rgba rows)})"""
    assert d[:4] == b'FRES'
    pool_info = _q(d, 0xb0)                                   # buffer memory pool: flags, size, offset
    buf_base = _q(d, pool_info + 8)
    emb = _q(d, 0xb8)                                         # embedded files: offset, size
    nmodels = struct.unpack_from('<H', d, 0xdc)[0]
    models = {}
    for mi in range(nmodels):
        m = _q(d, 0x28) + mi * 0x78
        name = _str(d, _q(d, m + 8))
        vtx_arr, shp_arr, mat_arr = _q(d, m + 0x20), _q(d, m + 0x28), _q(d, m + 0x38)
        nvtx, nshp, nmat = struct.unpack_from('<HHH', d, m + 0x68)
        mats = []
        for k in range(nmat):
            f = mat_arr + k * 0xa8
            tn = _q(d, f + 0x30)
            ntex = d[f + 0x9c]
            mats.append(dict(name=_str(d, _q(d, f + 8)), tex=[_str(d, _q(d, tn + 8 * i)) for i in range(ntex)]))
        vtxs = []
        for k in range(nvtx):
            v = vtx_arr + k * 0x58
            attr_arr, info_arr, stride_arr = _q(d, v + 8), _q(d, v + 0x30), _q(d, v + 0x38)
            boff, nattr, nbuf = struct.unpack_from('<IBB', d, v + 0x48)
            count = struct.unpack_from('<I', d, v + 0x50)[0]
            sizes = [struct.unpack_from('<I', d, info_arr + 0x10 * i)[0] for i in range(nbuf)]
            strides = [struct.unpack_from('<I', d, stride_arr + 0x10 * i)[0] for i in range(nbuf)]
            starts, o = [], buf_base + boff
            for sz in sizes:
                starts.append(o); o += sz; o = (o + 7) & ~7
            attrs = {}
            for a in range(nattr):
                ap = attr_arr + a * 0x10
                fmt = struct.unpack_from('>H', d, ap + 8)[0]
                off, bi = struct.unpack_from('<HH', d, ap + 0xc)
                attrs[_str(d, _q(d, ap))] = (fmt, starts[bi] + off, strides[bi])
            vtxs.append(dict(count=count, attrs=attrs))
        shapes = []
        for k in range(nshp):
            s = shp_arr + k * 0x60
            mesh = _q(d, s + 0x18)
            mat_i, _bone, vtx_i = struct.unpack_from('<HHH', d, s + 0x52)
            prim, ifmt, icount, ifirst = struct.unpack_from('<IIII', d, mesh + 0x24)
            ioff = struct.unpack_from('<I', d, mesh + 0x20)[0]
            vt = vtxs[vtx_i]
            pos = read_attr(d, vt['attrs']['_p0'], vt['count'])
            uvk = os.environ.get('UVSET', '_u0'); uv = read_attr(d, vt['attrs'][uvk], vt['count']) if uvk in vt['attrs'] else None
            isz = 2 if ifmt == 1 else 4
            idx = [struct.unpack_from('<H' if isz == 2 else '<I', d, buf_base + ioff + i * isz)[0] + ifirst for i in range(icount)]
            shapes.append(dict(name=_str(d, _q(d, s + 8)), pos=pos, uv=uv, idx=idx, material=mats[mat_i]))
        models[name] = shapes
    texs = {}
    if emb:
        eo, es = _q(d, emb), struct.unpack_from('<I', d, emb + 8)[0]
        bn = d[eo:eo + es]
        if bn[:4] == b'BNTX':
            for t in bntx_textures(bn):
                texs[t['name']] = (t, bn)
    return dict(models=models, textures=texs)


def read_attr(d, a, n):
    fmt, o, stride = a
    out = []
    for i in range(n):
        p = o + i * stride
        if fmt == 0x0518: out.append(struct.unpack_from('<fff', d, p))
        elif fmt == 0x0517: out.append(struct.unpack_from('<ff', d, p))
        elif fmt == 0x0515: out.append(tuple(float_half(x) for x in struct.unpack_from('<HHHH', d, p))[:3])
        elif fmt == 0x0512: out.append(tuple(float_half(x) for x in struct.unpack_from('<HH', d, p)))
        elif fmt == 0x0112: out.append(tuple(x / 65535 for x in struct.unpack_from('<HH', d, p)))
        elif fmt == 0x0212: out.append(tuple(max(-1, x / 32767) for x in struct.unpack_from('<hh', d, p)))
        else: raise ValueError('attribute format %04x' % fmt)
    return out


def float_half(h):
    return struct.unpack('<e', struct.pack('<H', h))[0]


# ---------------------------------------------------------------- textures (BC1 / BC3, mip of choice)
def _rgb565(c):
    return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)


def decode_texture(t, bn, max_size=256):
    """-> (w, h, rows of (r, g, b, a)) of the first mip no larger than max_size."""
    fmt = t['fmtname']
    level = 0
    while (t['w'] >> level) > max_size and level + 1 < t['mips']: level += 1
    w, h = max(1, t['w'] >> level), max(1, t['h'] >> level)
    bpp = 8 if fmt.startswith('BC1') or fmt.startswith('BC4') else 16
    bw, bh = (w + 3) // 4, (h + 3) // 4
    # block height for this mip: shrinks with the mip's height in GOBs
    blkh = t['bh']
    while blkh > 0 and bh <= (8 << (blkh - 1)) // 2: blkh -= 1
    data = bn[t['mipoffs'][level]:]
    lin = swz.deswizzle(data, bw, bh, bpp, blkh)
    rows = [[(0, 0, 0, 255)] * w for _ in range(h)]
    for by in range(bh):
        for bx in range(bw):
            b = lin[(by * bw + bx) * bpp:(by * bw + bx + 1) * bpp]
            if fmt.startswith('BC1'):
                c0, c1, bits = struct.unpack_from('<HHI', b)
                p0, p1 = _rgb565(c0), _rgb565(c1)
                if c0 > c1: pal = [p0 + (255,), p1 + (255,), tuple((2 * a + c) // 3 for a, c in zip(p0, p1)) + (255,), tuple((a + 2 * c) // 3 for a, c in zip(p0, p1)) + (255,)]
                else: pal = [p0 + (255,), p1 + (255,), tuple((a + c) // 2 for a, c in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
                for i in range(16):
                    x, y = bx * 4 + i % 4, by * 4 + i // 4
                    if x < w and y < h: rows[y][x] = pal[bits >> (2 * i) & 3]
            elif fmt.startswith('BC3'):
                a0, a1 = b[0], b[1]
                abits = int.from_bytes(b[2:8], 'little')
                apal = [a0, a1] + ([((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)] if a0 > a1 else [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255])
                c0, c1, bits = struct.unpack_from('<HHI', b, 8)
                p0, p1 = _rgb565(c0), _rgb565(c1)
                pal = [p0, p1, tuple((2 * a + c) // 3 for a, c in zip(p0, p1)), tuple((a + 2 * c) // 3 for a, c in zip(p0, p1))]
                for i in range(16):
                    x, y = bx * 4 + i % 4, by * 4 + i // 4
                    if x < w and y < h: rows[y][x] = pal[bits >> (2 * i) & 3] + (apal[abits >> (3 * i) & 7],)
            else:
                raise ValueError('texture format ' + fmt)
    return w, h, rows


# ---------------------------------------------------------------- rendering
def srgb_to_lin(c): c /= 255; return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
def lin_to_srgb(c): c = max(0.0, min(1.0, c)); return round(255 * (c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055))


def render(shapes, texs, size=300, ss=2, yaw=-35.0, pitch=28.0, fill=0.86, skip=('Snow', 'snow')):
    """Orthographic 3/4 view of the shapes, fitted to the picture; returns rows of RGBA."""
    W = size * ss
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    def view(p):
        x, y, z = p
        x, z = x * cy - z * sy, x * sy + z * cy          # yaw about Y
        y, z = y * cp - z * sp, y * sp + z * cp          # pitch about X (looking down)
        return x, y, -z                                  # depth: smaller = nearer the camera
    tris = []
    for s in shapes:
        if any(k in s['name'] or k in s['material']['name'] for k in skip): continue
        alb = next((t for t in s['material']['tex'] if t.endswith('_Alb')), s['material']['tex'][0] if s['material']['tex'] else None)
        tex = texs.get(alb)
        vp = [view(p) for p in s['pos']]
        for i in range(0, len(s['idx']) - 2, 3):
            a, b, c = s['idx'][i:i + 3]
            tris.append((vp[a], vp[b], vp[c], s['pos'][a], s['pos'][b], s['pos'][c],
                         s['uv'][a] if s['uv'] else (0, 0), s['uv'][b] if s['uv'] else (0, 0), s['uv'][c] if s['uv'] else (0, 0), tex))
    if not tris: return None
    xs = [v[0] for t in tris for v in t[:3]]; ys = [v[1] for t in tris for v in t[:3]]
    span = max(max(xs) - min(xs), max(ys) - min(ys))
    k = fill * W / span
    ox, oy = (max(xs) + min(xs)) / 2, (max(ys) + min(ys)) / 2
    base = {}
    for t in tris:
        tex = t[9]
        if tex is None or id(tex) in base: continue
        px = [p for row in tex[2][::4] for p in row[::4] if not (p[2] > p[0] + 50 and p[2] > 110) and sum(p[:3]) >= 24]
        px.sort(key=lambda p: p[0] + p[1] + p[2])
        base[id(tex)] = px[len(px) // 2][:3] if px else (170, 160, 150)
    zbuf = [[1e30] * W for _ in range(W)]
    col = [[None] * W for _ in range(W)]
    light = (-0.45, 0.8, 0.4); ln = math.sqrt(sum(v * v for v in light)); light = tuple(v / ln for v in light)
    for va, vb, vc, pa, pb, pc, ua, ub, uc, tex in tris:
        # face normal (model space) for lighting
        e1 = [pb[i] - pa[i] for i in range(3)]; e2 = [pc[i] - pa[i] for i in range(3)]
        n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        nl = math.sqrt(sum(v * v for v in n)) or 1
        shade = abs(sum(n[i] / nl * light[i] for i in range(3)))
        shade = 0.45 + 0.6 * shade
        P = [((v[0] - ox) * k + W / 2, W / 2 - (v[1] - oy) * k, v[2]) for v in (va, vb, vc)]
        x0, x1 = max(0, int(min(p[0] for p in P))), min(W - 1, int(max(p[0] for p in P)) + 1)
        y0, y1 = max(0, int(min(p[1] for p in P))), min(W - 1, int(max(p[1] for p in P)) + 1)
        (ax, ay, az), (bx, by, bz), (cx, cy2, cz) = P
        area = (bx - ax) * (cy2 - ay) - (cx - ax) * (by - ay)
        if abs(area) < 1e-9: continue
        tw = th = 0
        if tex: tw, th, trows = tex
        for y in range(y0, y1 + 1):
            fy = y + 0.5
            for x in range(x0, x1 + 1):
                fx = x + 0.5
                w0 = ((bx - fx) * (cy2 - fy) - (cx - fx) * (by - fy)) / area
                w1 = ((cx - fx) * (ay - fy) - (ax - fx) * (cy2 - fy)) / area
                w2 = 1 - w0 - w1
                if w0 < 0 or w1 < 0 or w2 < 0: continue
                z = w0 * az + w1 * bz + w2 * cz
                if z >= zbuf[y][x]: continue
                if w0 * pa[1] + w1 * pb[1] + w2 * pc[1] < CLIPY: continue
                if tex:
                    u = w0 * ua[0] + w1 * ub[0] + w2 * uc[0]
                    v = w0 * ua[1] + w1 * ub[1] + w2 * uc[1]
                    r, g, b, a = trows[int((1 - v) % 1 * th) % th if FLIPV else int(v % 1 * th) % th][int(u % 1 * tw) % tw]
                    if a < 128: continue
                    if (b > r + 50 and b > 110) or r + g + b < 24:   # the atlas' water-line gradient strip and
                        r, g, b = base[id(tex)]     # unused black areas: the material's plain colour instead
                else:
                    r, g, b = 180, 170, 160
                zbuf[y][x] = z
                col[y][x] = (r, g, b, shade)
    out = []
    for Y in range(size):
        row = []
        for X in range(size):
            acc = [0.0, 0.0, 0.0]; cov = 0
            for j in range(ss):
                for i in range(ss):
                    c = col[Y * ss + j][X * ss + i]
                    if c is None: continue
                    cov += 1
                    for ch in range(3): acc[ch] += srgb_to_lin(c[ch]) * c[3]
            if not cov: row.append((0, 0, 0, 0)); continue
            row.append(tuple(lin_to_srgb(a / cov) for a in acc) + (round(255 * cov / (ss * ss)),))
        out.append(row)
    return out


def write_png(path, rows):
    import zlib
    h, w = len(rows), len(rows[0])
    raw = b''.join(b'\0' + bytes(v for p in r for v in p) for r in rows)
    def ch(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))


def load_archive(romfs, name):
    import zstandard
    sys.path.insert(0, HERE)
    raw = zstandard.ZstdDecompressor().stream_reader(open(os.path.join(romfs, 'Model', name + '.Nin_NX_NVN.zs'), 'rb').read()).read()
    # SARC holding output.bfres
    hl = struct.unpack_from('<H', raw, 4)[0]; doff = struct.unpack_from('<I', raw, 0xc)[0]
    nn = struct.unpack_from('<H', raw, hl + 6)[0]
    s, e = struct.unpack_from('<II', raw, hl + 0xc + 8)
    return read_bfres(raw[doff + s:doff + e])


if __name__ == '__main__':
    romfs, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for arg in sys.argv[3:]:
        arch, _, model = arg.partition(':')
        res = load_archive(romfs, arch)
        texs = {n: decode_texture(t, bn) for n, (t, bn) in res['textures'].items() if n.endswith('_Alb')}
        for mname, shapes in res['models'].items():
            if model and mname != model: continue
            img = render(shapes, texs)
            if img: write_png(os.path.join(out, mname + '.png'), img); print('rendered', mname)
