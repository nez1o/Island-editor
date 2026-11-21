# RomFS side of the "Bridges" tab in Happy Home Paradise's house exterior menu (the menu the
# Island Editor opens with X on a house). Called by build_romfs.py; builds from the player's own
# dump, so no game files go into the repository.
#
#   Layout/WherearenIslandsCustomHome     a 6th tab button L_Category_05 (copy of L_Category_04,
#                                         one tab step to the right) and frame 5 of the
#                                         CategoryNum animation (layout for 6 tabs: frame = tabs - 1)
#   Layout/LWherearenIslandsCustomHomeCatBtn   tab icon texture IconCatBridge^s (pixel-drawn in
#                                         bridge_icons.tab_icon) and frame 5 of the Pattern animation
#                                         (frame = tab index) showing it
#   Message/String_<lang>                 STR_CategoryName label BRIDGES_LABEL = "Bridges" (the
#                                         bubble above the highlighted tab)
import os, struct, sys
import zstandard

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bflan, bntx_write, bc4, swz, bridge_icons

BRIDGES_LABEL = '1290'          # unused STR_CategoryName label (game's are 0001-1211)
TAB_TEXTURE = 'IconCatBridge^s'
TAB_STEP = 98.0                 # layout units between tab buttons (L_Category_0n x = 97 + 98 n)
NEW_TAB = 5                     # index of the Bridges tab


def zs_read(path):
    return zstandard.ZstdDecompressor().stream_reader(open(path, 'rb').read()).read()


def zs_write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'wb').write(zstandard.ZstdCompressor(level=19).compress(data))


# ---------------------------------------------------------------- bflyt
def flyt_sections(body):
    p = struct.unpack_from('<H', body, 6)[0]; cnt = struct.unpack_from('<H', body, 0x10)[0]
    for _ in range(cnt):
        sz = struct.unpack_from('<I', body, p + 4)[0]
        yield bytes(body[p:p + 4]), p, sz
        p += sz


def add_tab_pane(body):
    """Copy prt1 L_Category_04 as L_Category_05 right after it, and add it to G_CategoryNum_00."""
    body = bytearray(body)
    src = [(p, sz) for t, p, sz in flyt_sections(body)
           if t == b'prt1' and bytes(body[p + 0xc:p + 0x24]).split(b'\0')[0] == b'L_Category_04']
    (p, sz), = src
    pane = bytearray(body[p:p + sz])
    pane[0xc:0x24] = b'L_Category_05'.ljust(0x18, b'\0')
    x = struct.unpack_from('<f', pane, 0x2c)[0]
    struct.pack_into('<f', pane, 0x2c, x + TAB_STEP)
    body[p + sz:p + sz] = pane
    struct.pack_into('<H', body, 0x10, struct.unpack_from('<H', body, 0x10)[0] + 1)
    # group used by the CategoryNum animation (grp1: name[0x22], u16 count, names[24])
    for t, g, gsz in flyt_sections(body):
        if t == b'grp1' and bytes(body[g + 8:g + 0x2a]).split(b'\0')[0] == b'G_CategoryNum_00':
            n = struct.unpack_from('<H', body, g + 0x2a)[0]
            body[g + gsz:g + gsz] = b'L_Category_05'.ljust(24, b'\0')
            struct.pack_into('<H', body, g + 0x2a, n + 1)
            struct.pack_into('<I', body, g + 4, gsz + 24)
            break
    else:
        raise ValueError('G_CategoryNum_00 not found')
    struct.pack_into('<I', body, 0xc, len(body))
    return bytes(body)


# ---------------------------------------------------------------- animations
def extend_category_num(data):
    """CategoryNum: frame f lays the strip out for f + 1 tabs; add frame 5 (6 tabs) by carrying each
    stepped track one more step (same difference as from frame 3 to 4)."""
    a = bflan.read(data)
    pai = [s for mg, s in a['sections'] if mg == b'pai1'][0]
    assert pai['frames'] == 4
    pai['frames'] = 5
    for e in pai['entries']:
        for t in e['tags']:
            for inf in t['infos']:
                k = inf['keys']
                if inf['curve'] == 1:                       # visibility: L_Category_04 -> keep shown
                    continue
                at = {}
                for key in k: at.setdefault(key[0], []).append(key[1])
                if 4.0 not in at or 3.0 not in at: continue
                v4, v3 = at[4.0][-1], at[3.0][-1]
                new = v4 + (v4 - v3)
                if t['tag'] == 'FLTS':                      # texture scroll: alternates 0 / -0.5
                    new = v3
                k += [(5.0, v4, 0.0), (5.0, new, 0.0)]
    pai['entries'].append(dict(name='L_Category_05', kind=0, tags=[dict(tag='FLVI', infos=[
        dict(index=0, target=0, curve=1, keys=[(4.0, 0), (5.0, 1)])])]))
    return bflan.write(a)


def extend_pattern(data):
    """Pattern (tab icon): frame i -> icon texture of tab i; frame 5 -> the bridge."""
    a = bflan.read(data)
    pai = [s for mg, s in a['sections'] if mg == b'pai1'][0]
    pai['textures'].append(TAB_TEXTURE)
    tex = len(pai['textures']) - 1
    pai['frames'] = NEW_TAB
    for e in pai['entries']:
        for t in e['tags']:
            if t['tag'] == 'FLTP':
                t['infos'][0]['keys'].append((float(NEW_TAB), tex))
    return bflan.write(a)


def add_tab_texture(bntx):
    arc = bntx_write.read(bntx)
    tmpl = [t for t in arc['textures'] if t['name'] == 'IconCatRoof^s'][0]
    img = bridge_icons.tab_icon(128)
    linear = bc4.encode(img)
    from bntx import textures
    bh = [t for t in textures(bntx) if t['name'] == 'IconCatRoof^s'][0]['bh']
    region = bytearray(len(tmpl['data']))
    swz.swizzle_into(region, linear, 32, 32, 8, bh)
    arc['textures'].append(dict(name=TAB_TEXTURE, brti=tmpl['brti'], data=bytes(region), align=tmpl['align']))
    return bntx_write.write(arc)


# ---------------------------------------------------------------- build
def build(romfs, out, sarc_files, sarc_rebuild, sarc_read, sarc_write, msbt_read, msbt_write, langs=('USen', 'EUen')):
    """Writes the files into <out>; returns {rstb path: decompressed size}."""
    sizes = {}
    # menu layout
    name = 'WherearenIslandsCustomHome'
    d = zs_read(os.path.join(romfs, 'Layout', name + '.Nin_NX_NVN.zs'))
    (arc_name, arc), = list(sarc_files(d))
    inner = dict(sarc_files(arc))
    new_arc = sarc_rebuild(arc, {
        'blyt/%s.bflyt' % name: add_tab_pane(inner['blyt/%s.bflyt' % name]),
        'anim/%s_CategoryNum.bflan' % name: extend_category_num(inner['anim/%s_CategoryNum.bflan' % name]),
    })
    whole = sarc_rebuild(d, {arc_name: new_arc})
    zs_write(os.path.join(out, 'Layout', name + '.Nin_NX_NVN.zs'), whole)
    sizes['Layout/%s.Nin_NX_NVN' % name] = len(whole)
    # tab button
    name = 'LWherearenIslandsCustomHomeCatBtn'
    d = zs_read(os.path.join(romfs, 'Layout', name + '.Nin_NX_NVN.zs'))
    (arc_name, arc), = list(sarc_files(d))
    inner = dict(sarc_files(arc))
    new_arc = sarc_rebuild(arc, {
        'timg/__Combined.bntx': add_tab_texture(inner['timg/__Combined.bntx']),
        'anim/%s_Pattern.bflan' % name: extend_pattern(inner['anim/%s_Pattern.bflan' % name]),
    })
    whole = sarc_rebuild(d, {arc_name: new_arc})
    zs_write(os.path.join(out, 'Layout', name + '.Nin_NX_NVN.zs'), whole)
    sizes['Layout/%s.Nin_NX_NVN' % name] = len(whole)
    # tab name
    for lang in langs:
        src = os.path.join(romfs, 'Message', 'String_%s.sarc.zs' % lang)
        mult, files = sarc_read(zs_read(src))
        files = [list(f) for f in files]
        for f in files:
            if f[0] != 'STR_CategoryName.msbt': continue
            header, ngroups, asize, entries = msbt_read(f[1])
            assert not any(e[0] == BRIDGES_LABEL for e in entries)
            like = [e for e in entries if e[0] == '1201'][0]        # "Roof": same attributes
            entries.append([BRIDGES_LABEL, like[1], 'Bridges'.encode('utf-16le') + b'\0\0'])
            f[1] = msbt_write(header, ngroups, asize, entries)
        blob = sarc_write(mult, [tuple(f) for f in files])
        zs_write(os.path.join(out, 'Message', 'String_%s.sarc.zs' % lang), blob)
        sizes['Message/String_%s.sarc' % lang] = len(blob)
    return sizes
