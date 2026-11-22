# RomFS side of the "Bridges" and "Inclines" tabs in Happy Home Paradise's house exterior menu (the menu the
# Island Editor opens with X on a house). Called by build_romfs.py; builds from the player's own
# dump, so no game files go into the repository.
#
# Two extra tabs after the game's five (NEW_TABS): Bridges (5) and Inclines (6).
#   Layout/WherearenIslandsCustomHome     tab buttons L_Category_05 / 06 (copies of the one before,
#                                         one tab step to the right) and frames 5 / 6 of the
#                                         CategoryNum animation (layout for f + 1 tabs at frame f)
#   Layout/LWherearenIslandsCustomHomeCatBtn   tab icon textures (drawn in bridge_icons.py) and frames
#                                         5 / 6 of the Pattern animation (frame = tab index)
#   Message/String_<lang>                 STR_CategoryName labels 1290 "Bridges", 1291 "Inclines" (the
#                                         bubble above the highlighted tab)
import os, struct, sys
import zstandard

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bflan, bntx_write, bc4, swz, bridge_icons, astc, render_icons

TAB_STEP = 98.0                 # layout units between tab buttons (L_Category_0n x = 97 + 98 n)
# The extra tabs, after the game's five: (tab index, STR_CategoryName label (unused; the game's are
# 0001-1211), name, tab icon texture, icon drawing). Must match HomeTabs in main.cpp.
NEW_TABS = [
    (5, '1290', 'Bridges', 'IconCatBridge^s', bridge_icons.tab_icon),
    (6, '1291', 'Inclines', 'IconCatSlope^s', bridge_icons.incline_tab_icon),
]


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


def add_tab_panes(body):
    for tab, *_ in NEW_TABS: body = add_tab_pane(body, tab)
    return set_strip_static(body)


# The tab strip's panes as laid out for 7 tabs (CategoryNum's last frame). The menu doesn't play
# that animation on the main island, so the layout's own values (the 5-tab layout) are what shows.
STRIP_7 = {'N_Cat_00': ('x', 49.0), 'N_L_00': ('x', -49.0), 'N_R_00': ('x', 49.0),
           'N_L_03': ('x', -489.0), 'N_R_03': ('x', -391.0), 'N_L_04': ('x', -489.0), 'N_R_04': ('x', -391.0),
           'W_CategoryBase_00': ('w', 726.0)}


def set_strip_static(body):
    body = bytearray(body)
    seen = set()
    for t, p, sz in flyt_sections(body):
        if t not in (b'pan1', b'wnd1', b'pic1', b'prt1'): continue
        name = bytes(body[p + 0xc:p + 0x24]).split(b'\0')[0].decode()
        if name not in STRIP_7: continue
        what, v = STRIP_7[name]
        struct.pack_into('<f', body, p + (0x2c if what == 'x' else 0x4c), v)
        seen.add(name)
    assert seen == set(STRIP_7), set(STRIP_7) - seen
    return bytes(body)


def add_tab_pane(body, tab):
    """Copy prt1 L_Category_<tab-1> as L_Category_<tab> right after it (one tab step right), and add
    it to G_CategoryNum_00."""
    body = bytearray(body)
    prev, name = ('L_Category_%02d' % (tab - 1)).encode(), ('L_Category_%02d' % tab).encode()
    src = [(p, sz) for t, p, sz in flyt_sections(body)
           if t == b'prt1' and bytes(body[p + 0xc:p + 0x24]).split(b'\0')[0] == prev]
    (p, sz), = src
    pane = bytearray(body[p:p + sz])
    pane[0xc:0x24] = name.ljust(0x18, b'\0')
    x = struct.unpack_from('<f', pane, 0x2c)[0]
    struct.pack_into('<f', pane, 0x2c, x + TAB_STEP)
    body[p + sz:p + sz] = pane
    struct.pack_into('<H', body, 0x10, struct.unpack_from('<H', body, 0x10)[0] + 1)
    # group used by the CategoryNum animation (grp1: name[0x22], u16 count, names[24])
    for t, g, gsz in flyt_sections(body):
        if t == b'grp1' and bytes(body[g + 8:g + 0x2a]).split(b'\0')[0] == b'G_CategoryNum_00':
            n = struct.unpack_from('<H', body, g + 0x2a)[0]
            body[g + gsz:g + gsz] = name.ljust(24, b'\0')
            struct.pack_into('<H', body, g + 0x2a, n + 1)
            struct.pack_into('<I', body, g + 4, gsz + 24)
            break
    else:
        raise ValueError('G_CategoryNum_00 not found')
    struct.pack_into('<I', body, 0xc, len(body))
    return bytes(body)


# ---------------------------------------------------------------- animations
def extend_category_num(data):
    """CategoryNum: frame f lays the strip out for f + 1 tabs; add one frame per new tab by carrying
    each stepped track one more step (same difference as between the two frames before)."""
    a = bflan.read(data)
    pai = [s for mg, s in a['sections'] if mg == b'pai1'][0]
    assert pai['frames'] == 4
    for tab, *_ in NEW_TABS:
        f = float(tab)
        pai['frames'] = tab
        for e in pai['entries']:
            for t in e['tags']:
                for inf in t['infos']:
                    k = inf['keys']
                    if inf['curve'] == 1:                   # visibility: earlier tabs stay shown
                        continue
                    at = {}
                    for key in k: at.setdefault(key[0], []).append(key[1])
                    if f - 1 not in at or f - 2 not in at: continue
                    v1, v2 = at[f - 1][-1], at[f - 2][-1]
                    new = v1 + (v1 - v2)
                    if t['tag'] == 'FLTS':                  # texture scroll: alternates 0 / -0.5
                        new = v2
                    k += [(f, v1, 0.0), (f, new, 0.0)]
        pai['entries'].append(dict(name='L_Category_%02d' % tab, kind=0, tags=[dict(tag='FLVI', infos=[
            dict(index=0, target=0, curve=1, keys=[(f - 1, 0), (f, 1)])])]))
    # The HHP menu sets this animation to its 5-tab frame (4) itself, whatever the tab count (the
    # module can't reach that call), so frame 4 shows the full 7-tab layout too. (In Happy Home
    # Paradise's own menu the two extra slots then show as well.)
    last = float(NEW_TABS[-1][0])
    for e in pai['entries']:
        for t in e['tags']:
            for inf in t['infos']:
                if inf['curve'] == 1:
                    if e['name'].startswith('L_Category_') and int(e['name'][-2:]) >= 5:
                        inf['keys'] = [(3.0, 0), (4.0, 1)]
                    continue
                k = inf['keys']
                if not any(key[0] == last for key in k): continue
                final = [key for key in k if key[0] == last][-1][1]
                inf['keys'] = [key for key in k if key[0] < 4.0] + [(4.0, [key for key in k if key[0] <= 4.0][-1][1], 0.0), (4.0, final, 0.0), (last, final, 0.0)]
    return bflan.write(a)


def extend_pattern(data):
    """Pattern (tab icon): frame i -> icon texture of tab i; one frame per new tab."""
    a = bflan.read(data)
    pai = [s for mg, s in a['sections'] if mg == b'pai1'][0]
    for tab, _label, _name, texture, _draw in NEW_TABS:
        pai['textures'].append(texture)
        tex = len(pai['textures']) - 1
        pai['frames'] = tab
        for e in pai['entries']:
            for t in e['tags']:
                if t['tag'] == 'FLTP':
                    t['infos'][0]['keys'].append((float(tab), tex))
    return bflan.write(a)


def add_tab_textures(bntx):
    arc = bntx_write.read(bntx)
    tmpl = [t for t in arc['textures'] if t['name'] == 'IconCatRoof^s'][0]
    from bntx import textures
    bh = [t for t in textures(bntx) if t['name'] == 'IconCatRoof^s'][0]['bh']
    for _tab, _label, _name, texture, draw in NEW_TABS:
        linear = bc4.encode(bridge_icons.fit_mask(draw(128)))   # same size / place as the game's icons
        region = bytearray(len(tmpl['data']))
        swz.swizzle_into(region, linear, 32, 32, 8, bh)
        arc['textures'].append(dict(name=texture, brti=tmpl['brti'], data=bytes(region), align=tmpl['align']))
    return bntx_write.write(arc)


# ---------------------------------------------------------------- cell pictures
# The menu's cells show prerendered icon archives, Model/Layout_<kind>Icon_<name>; the house shapes
# use "Layout_HouseShapeIcon_%s" (300x300 ASTC 4x4 sRGB, name "00".."07"). Each bridge / incline look
# gets one rendered from the game's own model (render_icons.py), packed into a copy of
# Layout_HouseShapeIcon_00 with the two-letter name below (same length, so only the name bytes and
# the pixels change). main.cpp asks for them with the house-shape icon kind.
BRIDGE_ICONS = [   # grid order (main.cpp k_BridgeIds): archive, model
    ('BridgeLog', 'BridgeLog04'), ('BridgeSuspension', 'BridgeSuspension04'), ('BridgeStone', 'BridgeStone04'),
    ('BridgeWood', 'BridgeWood04'), ('BridgeBricks', 'BridgeBricks04'), ('BridgeIron', 'BridgeIron04'),
    ('BridgeJapanese', 'BridgeJapanese04'), ('BridgeRed', 'BridgeRed04')]
INCLINE_ICONS = [  # grid order (main.cpp k_InclineIds)
    ('SlopeNatural', 'SlopeNatural'), ('SlopeWood', 'SlopeWood'), ('SlopeWoodStair', 'SlopeWoodStair'),
    ('SlopeStoneStair', 'SlopeStoneStair'), ('SlopeBrickStair', 'SlopeBrickStair'), ('SlopeIronStair', 'SlopeIronStair'),
    ('SlopeWoodBlue', 'SlopeWoodBlue'), ('SlopeIronStairBlue', 'SlopeIronStairBlue')]
ICON_TEMPLATE = 'Layout_HouseShapeIcon_00'
# Bridges whose deck the shader layers in game (render_icons can't): planks drawn from this atlas
# rect (u0, v0, u1, v1), tint, height below the top the deck stays (None: every upward face).
# Materials the game tints at run time (the natural incline's mossy step stones are grey in game).
RECOLOR = {'SlopeNatural': {'SlopeNaturalStone': (168, 160, 148)},
           'SlopeWood': {'mSlopeWood': (138, 100, 66)}}   # weathered grey-green planks -> wood
DECKS = {
    'BridgeIron': (0.02, 0.58, 0.36, 0.98, (165, 135, 122), 3.0),   # matched to an in-game screenshot
    'BridgeWood': (0.0, 0.45, 0.48, 0.98, None),
    'BridgeSuspension': (0.0, 0.58, 0.48, 0.98, None),
}


def icon_names():
    return [('B%d' % i, a, m) for i, (a, m) in enumerate(BRIDGE_ICONS)] + [('S%d' % i, a, m) for i, (a, m) in enumerate(INCLINE_ICONS)]


def build_icons(romfs, out, sarc_files, sarc_rebuild, preview_dir=None, pictures=None):
    """pictures: a folder of 300x300 PNGs named B0..B7 / S0..S7 to use instead of renders (env
    BRIDGE_PICTURES for build_romfs.py); kept outside the repository."""
    """Writes Model/Layout_HouseShapeIcon_<B0..B7, S0..S7>; returns {rstb path: size} (new entries)."""
    tpl = zs_read(os.path.join(romfs, 'Model', ICON_TEMPLATE + '.Nin_NX_NVN.zs'))
    (bf_name, bf), = list(sarc_files(tpl))
    emb = struct.unpack_from('<Q', bf, 0xb8)[0]
    eo, es = struct.unpack_from('<Q', bf, emb)[0], struct.unpack_from('<I', bf, emb + 8)[0]
    from bntx import textures
    t, = textures(bf[eo:eo + es])
    assert t['name'] == '00' and (t['w'], t['h']) == (300, 300) and t['fmt'] == 0x2d06, t
    name_at = bf.index(b'\x02\x0000\x00')
    cache = {}
    sizes = {}
    for name, arch, model in icon_names():
        pic = os.path.join(pictures, name + '.png') if pictures else None
        if pic and os.path.exists(pic):     # a 300x300 picture supplied by the player (the shop's own previews)
            import png
            pw, ph, img = png.read(pic)
            assert (pw, ph) == (300, 300), pic
        else:                                # else render the game's model
            if arch not in cache:
                res = render_icons.load_archive(romfs, arch)
                texs = {n: render_icons.decode_texture(tx, bn) for n, (tx, bn) in res['textures'].items() if n.endswith('_Alb') or n.endswith('_Mix')}
                cache = {arch: (res, texs)}
            res, texs = cache[arch]
            img = render_icons.render(res['models'][model], texs, size=300,
                                      deck=DECKS.get(arch), recolor=RECOLOR.get(arch),
                                      scene='bridge' if arch.startswith('Bridge') else 'slope',
                                      yaw=-125.0 if arch.startswith('Bridge') else -35.0, pitch=32.0)
        if preview_dir: render_icons.write_png(os.path.join(preview_dir, name + '.png'), img)
        linear = astc.encode(img, 4, 4)
        region = bytearray(t['size'])
        swz.swizzle_into(region, linear, 75, 75, 16, t['bh'])
        nb = bytearray(bf)
        nb[name_at + 2:name_at + 4] = name.encode()
        nb[eo + t['data']:eo + t['data'] + t['size']] = region
        whole = sarc_rebuild(tpl, {bf_name: bytes(nb)})
        path = 'Model/Layout_HouseShapeIcon_%s.Nin_NX_NVN' % name
        zs_write(os.path.join(out, path + '.zs'), whole)
        sizes[path] = len(whole)
        print('icon', name, arch, model)
    return sizes


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
        'blyt/%s.bflyt' % name: add_tab_panes(inner['blyt/%s.bflyt' % name]),
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
        'timg/__Combined.bntx': add_tab_textures(inner['timg/__Combined.bntx']),
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
            like = [e for e in entries if e[0] == '1201'][0]        # "Roof": same attributes
            for _tab, label, text, _tex, _draw in NEW_TABS:
                assert not any(e[0] == label for e in entries)
                entries.append([label, like[1], text.encode('utf-16le') + b'\0\0'])
            f[1] = msbt_write(header, ngroups, asize, entries)
        blob = sarc_write(mult, [tuple(f) for f in files])
        zs_write(os.path.join(out, 'Message', 'String_%s.sarc.zs' % lang), blob)
        sizes['Message/String_%s.sarc' % lang] = len(blob)
    sizes.update(build_icons(romfs, out, sarc_files, sarc_rebuild, pictures=os.environ.get('BRIDGE_PICTURES')))
    return sizes
