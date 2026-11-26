# RomFS side of the island editor's D-pad left guide: the editor's D-pad guide (top left, layout
# Layout/InteriorWherearenIslands) shows a greyed-out left arrow on the main island. D-pad left opens
# the module's spawn menu there, so the left slot gets a lit arrow and a "spawn" icon (a bridge with
# a plus), drawn here from simple shapes (no game art).
#
# How the slot works (found in the dump, checked live on a Switch): animation
# InteriorWherearenIslands_KeyLeft (3 frames, step keys; the game re-applies it, a poke of the pane's
# visible bit was undone) picks what the left slot shows. Two D-pad guide variants:
#   N_TypeInside_05 (HHP interiors): frame 0 grey arrow N_KeyLeftOff_00, frame 1 N_KeyLeft_00 (lit
#     arrow + P_InteriorModePlus_00)
#   N_Type_06 (exteriors, what the island editor shows on the main island; sun icon on top):
#     frame 0 nothing lit, frame 2 N_KeyLeft_02 (lit arrow P_JujiL_06 + P_Season_00, HHP seasons)
# The island editor runs at frame 0. Changes, for both variants:
#   - frame 0 shows the lit slot (N_KeyLeft_00 / N_KeyLeft_02), its icon gets a texture pattern (FLTP)
#     frame 0 = new IconSpawnDpad^s, other frames = the game's own icon; the icon panes join group
#     G_KeyLeft_00 (the animation binds through it); layout defaults match frame 0
#   - bntx: the new texture (BC4 128x128, a mask the layout tints, like the game's icons)
# Side effect: anywhere else frame 0 shows (HHP interiors before "plus" mode is unlocked, HHP
# exteriors before seasons are), the spawn icon shows too.
#
# usage (standalone): python3 tools/dpad_icon.py <RomFS dir> <out romfs dir> [preview.png]
#   writes <out>/Layout/InteriorWherearenIslands.Nin_NX_NVN.zs and raises its entry in
#   <out>/System/Resource/ResourceSizeTable.srsizetable (which must already be there).
# build_romfs.py calls build() and handles the size table itself.
import os, struct, sys
import zstandard

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bflan, bntx_write, bc4, swz, bridge_icons

LAYOUT = 'InteriorWherearenIslands'
TEXTURE = 'IconSpawnDpad^s'
TEMPLATE = 'IconCatInteriorPlus^s'      # format / size template for the new texture
# (lit slot pane, its icon pane = material, the game's icon texture, frame where the game shows it)
SLOTS = [('N_KeyLeft_00', 'P_InteriorModePlus_00', 'IconCatInteriorPlus^s', 1),
         ('N_KeyLeft_02', 'P_Season_00', 'GuideIconSeasons^s', 2)]


def zs_read(path):
    return zstandard.ZstdDecompressor().stream_reader(open(path, 'rb').read()).read()


def zs_write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'wb').write(zstandard.ZstdCompressor(level=19).compress(data))


# ---------------------------------------------------------------- icon
def spawn_icon(size=128, ss=4):
    """0-255 mask: the Bridges tab's bridge (smaller, bottom left) and a plus sign (top right)."""
    bridge = bridge_icons.fit_mask(bridge_icons.tab_icon(size, ss), box=(6, 44, 96, 117))
    N = size * ss
    cx, cy, arm, half = 0.80 * N, 0.22 * N, 0.17 * N, 0.055 * N
    def plus(x, y):
        return (abs(x - cx) <= half and abs(y - cy) <= arm) or (abs(y - cy) <= half and abs(x - cx) <= arm)
    img = []
    for y in range(size):
        row = []
        for x in range(size):
            c = sum(plus(x * ss + i + 0.5, y * ss + j + 0.5) for i in range(ss) for j in range(ss))
            row.append(max(bridge[y][x], round(255 * c / (ss * ss))))
        img.append(row)
    return img


def add_texture(bntx):
    arc = bntx_write.read(bntx)
    tmpl = [t for t in arc['textures'] if t['name'] == TEMPLATE][0]
    from bntx import textures
    info = [t for t in textures(bntx) if t['name'] == TEMPLATE][0]
    assert info['fmt'] == 0x1d01 and (info['w'], info['h']) == (128, 128), info
    linear = bc4.encode(spawn_icon(128))
    region = bytearray(len(tmpl['data']))
    swz.swizzle_into(region, linear, 32, 32, 8, info['bh'])
    arc['textures'].append(dict(name=TEXTURE, brti=tmpl['brti'], data=bytes(region), align=tmpl['align']))
    return bntx_write.write(arc)


# ---------------------------------------------------------------- bflyt
def sections(body):
    p = struct.unpack_from('<H', body, 6)[0]; cnt = struct.unpack_from('<H', body, 0x10)[0]
    for _ in range(cnt):
        sz = struct.unpack_from('<I', body, p + 4)[0]
        yield bytes(body[p:p + 4]), p, sz
        p += sz


def patch_layout(body):
    body = bytearray(body)
    # txl1: append the texture name
    (p, sz), = [(p, sz) for t, p, sz in sections(body) if t == b'txl1']
    n = struct.unpack_from('<H', body, p + 8)[0]
    base = p + 0xc
    offs = [struct.unpack_from('<I', body, base + 4 * i)[0] for i in range(n)]
    names = [bytes(body[base + o:]).split(b'\0')[0] for o in offs]
    assert TEXTURE.encode() not in names
    names.append(TEXTURE.encode())
    tex_index = len(names) - 1
    table = bytearray(); strings = bytearray()
    for nm in names:
        table += struct.pack('<I', 4 * len(names) + len(strings))
        strings += nm + b'\0'
    sec = bytearray(b'txl1' + b'\0' * 4) + struct.pack('<HH', len(names), 0) + table + strings
    sec += b'\0' * (-len(sec) % 4)
    struct.pack_into('<I', sec, 4, len(sec))
    body[p:p + sz] = sec
    # mat1: each icon's first texture map (u16 index right after the 0x10 bytes of flags / colours
    # that follow the 0x1c name) -> the new texture
    (p, sz), = [(p, sz) for t, p, sz in sections(body) if t == b'mat1']
    n = struct.unpack_from('<H', body, p + 8)[0]
    done = set()
    for i in range(n):
        o = p + struct.unpack_from('<I', body, p + 0xc + 4 * i)[0]
        mname = bytes(body[o:o + 0x1c]).split(b'\0')[0].decode()
        for _slot, mat, tex, _f in SLOTS:
            if mname == mat:
                assert struct.unpack_from('<H', body, o + 0x2c)[0] == names.index(tex.encode()), mat
                struct.pack_into('<H', body, o + 0x2c, tex_index)
                done.add(mat)
    assert done == {m for _s, m, _t, _f in SLOTS}, done
    # default visibility (pane flag byte +8, bit 0), as at frame 0
    seen = set()
    for t, p, sz in sections(body):
        if t != b'pan1': continue
        name = bytes(body[p + 0xc:p + 0x24]).split(b'\0')[0].decode()
        if name in [sl for sl, *_ in SLOTS]: body[p + 8] |= 1; seen.add(name)
        if name == 'N_KeyLeftOff_00': body[p + 8] &= ~1; seen.add(name)
    assert len(seen) == len(SLOTS) + 1, seen
    # group G_KeyLeft_00 (grp1: name[0x22], u16 count, names[24]): add the icon panes
    for t, g, gsz in sections(body):
        if t == b'grp1' and bytes(body[g + 8:g + 0x2a]).split(b'\0')[0] == b'G_KeyLeft_00':
            cnt = struct.unpack_from('<H', body, g + 0x2a)[0]
            add = b''.join(m.encode().ljust(24, b'\0') for _s, m, _t, _f in SLOTS)
            body[g + gsz:g + gsz] = add
            struct.pack_into('<H', body, g + 0x2a, cnt + len(SLOTS))
            struct.pack_into('<I', body, g + 4, gsz + len(add))
            break
    else:
        raise ValueError('G_KeyLeft_00 not found')
    struct.pack_into('<I', body, 0xc, len(body))
    return bytes(body)


# ---------------------------------------------------------------- animation
def patch_key_left(data):
    a = bflan.read(data)
    pai = [s for mg, s in a['sections'] if mg == b'pai1'][0]
    assert pai['frames'] == 2 and not pai['textures']
    keys = {'N_KeyLeft_00': [(0.0, 1), (2.0, 0)],             # was 0 / 1 / 0
            'N_KeyLeftOff_00': [(0.0, 0)],                      # was 1 / 0
            'N_KeyLeft_02': [(0.0, 1), (1.0, 0), (2.0, 1)]}     # was - / 0 / 1
    done = set()
    for e in pai['entries']:
        if e['name'] in keys:
            e['tags'][0]['infos'][0]['keys'] = keys[e['name']]; done.add(e['name'])
    assert done == set(keys), done
    pai['textures'] = [TEXTURE] + [tex for _s, _m, tex, _f in SLOTS]
    for i, (_s, mat, _tex, _f) in enumerate(SLOTS):
        pai['entries'].append(dict(name=mat, kind=1, tags=[dict(tag='FLTP', infos=[
            dict(index=0, target=0, curve=1, keys=[(0.0, 0), (1.0, i + 1)])])]))
    return bflan.write(a)


# ---------------------------------------------------------------- build
def build(romfs, out, sarc_files, sarc_rebuild):
    """Writes the layout into <out>; returns {rstb path: decompressed size}."""
    d = zs_read(os.path.join(romfs, 'Layout', LAYOUT + '.Nin_NX_NVN.zs'))
    (arc_name, arc), = list(sarc_files(d))
    inner = dict(sarc_files(arc))
    new_arc = sarc_rebuild(arc, {
        'blyt/%s.bflyt' % LAYOUT: patch_layout(inner['blyt/%s.bflyt' % LAYOUT]),
        'anim/%s_KeyLeft.bflan' % LAYOUT: patch_key_left(inner['anim/%s_KeyLeft.bflan' % LAYOUT]),
        'timg/__Combined.bntx': add_texture(inner['timg/__Combined.bntx']),
    })
    whole = sarc_rebuild(d, {arc_name: new_arc})
    zs_write(os.path.join(out, 'Layout', LAYOUT + '.Nin_NX_NVN.zs'), whole)
    return {'Layout/%s.Nin_NX_NVN' % LAYOUT: len(whole)}


# ---------------------------------------------------------------- standalone
def _sarc_files(buf):
    hl = struct.unpack_from('<H', buf, 4)[0]; doff = struct.unpack_from('<I', buf, 0xc)[0]
    nn = struct.unpack_from('<H', buf, hl + 6)[0]
    sfnt = hl + 0xc + nn * 16
    for i in range(nn):
        h, attr, s, e = struct.unpack_from('<IIII', buf, hl + 0xc + i * 16)
        yield bytes(buf[sfnt + 8 + (attr & 0xffff) * 4:]).split(b'\0')[0].decode(), bytes(buf[doff + s:doff + e])


def _sarc_rebuild(buf, replace):
    hl, bom, total, doff, ver = struct.unpack_from('<HHIIH', buf, 4)
    nn = struct.unpack_from('<H', buf, hl + 6)[0]
    sfat = hl + 0xc
    sfnt = sfat + nn * 16
    entries = []
    for i in range(nn):
        h, attr, s, e = struct.unpack_from('<IIII', buf, sfat + i * 16)
        name = bytes(buf[sfnt + 8 + (attr & 0xffff) * 4:]).split(b'\0')[0].decode()
        align = 4
        while align < 0x2000 and (doff + s) % (align * 2) == 0: align *= 2
        entries.append([i, name, s, e, align])
    out = bytearray(buf[:doff])
    pos = 0
    for i, name, s, e, align in sorted(entries, key=lambda x: x[2]):
        blob = replace.get(name, bytes(buf[doff + s:doff + e]))
        pos += -(doff + pos) % align
        out += b'\0' * (doff + pos - len(out))
        struct.pack_into('<II', out, sfat + i * 16 + 8, pos, pos + len(blob))
        out += blob; pos += len(blob)
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)


def raise_rstb(path, sizes):
    import yaz0, zlib
    d = bytearray(yaz0.decompress(open(path, 'rb').read()))
    assert d[:4] == b'RSTC'
    ncrc = struct.unpack_from('<I', d, 4)[0]
    index = {struct.unpack_from('<I', d, 12 + i * 12)[0]: 12 + i * 12 for i in range(ncrc)}
    for name, size in sizes.items():
        off = index[zlib.crc32(name.encode())]
        old = struct.unpack_from('<I', d, off + 4)[0]
        new = max(old, (size + 0x400 + 0x1f) & ~0x1f)
        struct.pack_into('<I', d, off + 4, new)
        print('RSTB %s: %d -> %d (file %d)' % (name, old, new, size))
    open(path, 'wb').write(yaz0.compress(bytes(d)))


if __name__ == '__main__':
    romfs, out = sys.argv[1], sys.argv[2]
    if len(sys.argv) > 3:   # zoomed preview of the icon
        import render_icons
        render_icons.write_png(sys.argv[3], [[(v, v, v, 255) for v in row for _ in range(4)] for row in spawn_icon() for _ in range(4)])
    sizes = build(romfs, out, _sarc_files, _sarc_rebuild)
    rstb = os.path.join(out, 'System', 'Resource', 'ResourceSizeTable.srsizetable')
    raise_rstb(rstb, sizes)
    print('wrote', sizes)
