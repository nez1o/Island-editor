# Build SlumberToggle's romfs: the "Slumber settings" menu with ON/OFF switches, reachable from the
# Island Designer app and from its own phone app ("Gyroid's menu"; the module draws that app's own
# screen and only falls back to this talk menu when it can't).
#
#   Message/TalkSys_<lang>.sarc.zs   SYS_PhoneApp.msbt + our menu/status texts
#   Message/LayoutMsg_<lang>.sarc.zs MenuDevice.msbt: the phone app's name (label 0090)
#   EventFlow/Player_ConstructionLicenseApp.bfevfl
#       - both Island Designer menus (201 idle / 205 building) get a "Slumber settings." choice
#       - new entry point "SlumberApp" (started by the phone app) opens the same menu directly
#       - the submenu toggles island flags "cLand:SLT_<key>"; the subsdk8 module intercepts
#         those flag reads/writes and flips the matching feature (see main.cpp)
#   Layout/LMenuDeviceBtn.Nin_NX_NVN.zs   the phone app icon (see phone_icon.py), same file size
#   Stage/Game/MainField.byml   the Slumber HUD as a resident UI scene of the main island (stage_hud.py)
#   Layout/WherearenIslandsCustomHome, LWherearenIslandsCustomHomeCatBtn, Message/String_<lang>:
#       the island editor's "Bridges" tab in the HHP house menu (bridges_tab.py)
#
# usage: build_romfs.py <RomFS dir> <base Player_ConstructionLicenseApp.bfevfl> <out romfs dir>
#                       [<Aden Modmenu romfs dir> <out dir for Aden-merged copies>]
# Aden Modmenu serves its own LMenuDeviceBtn + LayoutMsg from sd:/aden/romfs; with the extra arguments
# the phone app changes are also applied on top of Aden's copies so both apps show when Aden is installed.
import sys, os, struct, io, zstandard

HERE = os.path.dirname(os.path.abspath(__file__))
import evfl                     # pip package "evfl" (work-tools\lib2 on PYTHONPATH)
from evfl.common import StringHolder
from evfl.container import Container

ROMFS, BASE_FLOW, OUT = sys.argv[1:4]
ADEN_ROMFS, ADEN_OUT = (sys.argv[4:6] + [None, None])[:2]

# Must match the keys in source/program/patches.inc (gen_patches.py FEATURES).
FEATURES = [
    ('slumber_dpad',            'Slumber catalog + time'),
    ('instant_bridges',         'Instant bridges/inclines'),
    ('trees_anywhere',          'Trees anywhere'),
    ('outdoor_rugs',            'Rugs outdoors'),
    ('outdoor_partition_walls', 'Partition walls outdoors'),
    ('level4_access',           'Level-4 cliff access'),
    ('slumber_terraform',       'Slumber terraforming'),
    ('terraform_near_trees',    'Terraform near trees/cliffs'),
    ('slumber_weather',         'Slumber weather (rain)'),
    ('slumber_phone_app',       'Slumber phone app'),
    ('fast_terraform',          'Faster terraforming'),
    ('slumber_hud',             'Slumber HUD + modes (+)'),
    ('island_editor',           'Island editor app'),
]
# A question has at most 6 choices: the first page has 4 switches + "More" + "Done", middle pages
# 3 switches + "More" + "Back" + "Done", the last page its switches + "Back" + "Done".
PAGE_SIZES = [4, 3, 3, 3]
PAGES = []
_i = 0
for _n in PAGE_SIZES:
    PAGES.append(FEATURES[_i:_i + _n]); _i += _n
assert _i == len(FEATURES)

# Message labels in SYS_PhoneApp. Must look like the game's own (numeric, "<question>_<letter>"
# choices) - choices of a non-numeric label ("SLT_1_a") showed up blank in-game.
def msg_page(p): return '%d' % (601 + p)
def msg_on(i): return '%d' % (640 + i)    # status lines after a switch (up to 20 switches)
def msg_off(i): return '%d' % (660 + i)
# MenuDevice.msbt labels of the phone apps' names (the module points the app records at them).
APP_NAMES = [('0090', "Gyroid's menu"), ('0091', 'Island Editor')]

# ---------------------------------------------------------------- messages
def msbt_read(b):
    secs = []; o = 0x20
    while o < len(b):
        mg, sz = b[o:o + 4], struct.unpack_from('<I', b, o + 4)[0]
        secs.append((mg, b[o + 0x10:o + 0x10 + sz])); o += 0x10 + sz; o = (o + 15) & ~15
    sec = dict(secs)
    lbl = sec[b'LBL1']; ngroups = struct.unpack_from('<I', lbl, 0)[0]
    labels = {}
    for i in range(ngroups):
        cnt, off = struct.unpack_from('<II', lbl, 4 + i * 8)
        for _ in range(cnt):
            ln = lbl[off]; name = lbl[off + 1:off + 1 + ln].decode(); idx = struct.unpack_from('<I', lbl, off + 1 + ln)[0]
            labels[idx] = name; off += 1 + ln + 4
    atr = sec[b'ATR1']; acount, asize = struct.unpack_from('<II', atr, 0)
    txt = sec[b'TXT2']; tn = struct.unpack_from('<I', txt, 0)[0]
    offs = list(struct.unpack_from('<%dI' % tn, txt, 4)) + [len(txt)]
    entries = []
    for i in range(tn):
        entries.append([labels[i], atr[8 + i * asize:8 + (i + 1) * asize], txt[offs[i]:offs[i + 1]]])
    return b[:0x20], ngroups, asize, entries

def label_hash(s, n):
    h = 0
    for c in s.encode():
        h = (h * 0x492 + c) & 0xffffffff
    return h % n

def section(magic, data):
    out = magic + struct.pack('<I', len(data)) + b'\0' * 8 + data
    return out + b'\xab' * (-len(out) % 16)

def msbt_write(header, ngroups, asize, entries):
    groups = [[] for _ in range(ngroups)]
    for i, (lab, _, _) in enumerate(entries):
        groups[label_hash(lab, ngroups)].append((lab, i))
    lbl = bytearray(struct.pack('<I', ngroups))
    body = bytearray(); base = 4 + ngroups * 8
    for g in groups:
        lbl += struct.pack('<II', len(g), base + len(body))
        for lab, i in g:
            body += bytes([len(lab)]) + lab.encode() + struct.pack('<I', i)
    lbl += body
    atr = struct.pack('<II', len(entries), asize) + b''.join(e[1] for e in entries)
    txt = bytearray(struct.pack('<I', len(entries))); data = bytearray(); base = 4 + 4 * len(entries)
    for e in entries:
        txt += struct.pack('<I', base + len(data)); data += e[2]
    txt += data
    out = bytearray(header) + section(b'LBL1', bytes(lbl)) + section(b'ATR1', atr) + section(b'TXT2', bytes(txt))
    struct.pack_into('<H', out, 0xe, 3)
    struct.pack_into('<I', out, 0x12, len(out))
    return bytes(out)

def u16(s):
    return s.encode('utf-16-le') + b'\0\0'

def question_attr(base, choices):
    # ATR1 byte 0 = 2 * (number of choices - 1) for a question, 0 for a plain message.
    return bytes([2 * (choices - 1) if choices else 0]) + base[1:]

def patch_messages(entries):
    by = {e[0]: e for e in entries}
    c_attr = by['201_a'][1]
    def setm(lab, text, attr):
        if lab in by:
            by[lab][2] = u16(text); by[lab][1] = attr
            return
        e = [lab, attr, u16(text)]; by[lab] = e
        # The game takes a question's choices from the entries right after it, so a new
        # choice ("201_e") goes directly after its last existing sibling ("201_d").
        stem = lab.rsplit('_', 1)[0]
        siblings = [i for i, x in enumerate(entries) if x[0] == stem or x[0].startswith(stem + '_')]
        if '_' in lab and siblings:
            entries.insert(siblings[-1] + 1, e)
        else:
            # New questions/messages use numeric labels like the game's own and are kept in
            # ascending order, i.e. before the file's last entry ("999").
            pos = next((i for i, x in enumerate(entries) if x[0] > lab and '_' not in x[0]), len(entries))
            entries.insert(pos, e)
    q_attr = None  # set per message below
    plain = question_attr(by['201'][1], 0)
    for p, page in enumerate(PAGES):
        choices = [label + '.' for _, label in page]
        if p < len(PAGES) - 1: choices.append('More settings...')
        if p > 0: choices.append('Back.')
        choices.append("I'm done.")
        setm(msg_page(p), 'Which Slumber setting should I switch?', question_attr(c_attr, len(choices)))
        for c, text in enumerate(choices):
            setm(msg_page(p) + '_' + 'abcdef'[c], text, c_attr)
    for i, (key, label) in enumerate(FEATURES):
        setm(msg_on(i), '%s: ON' % label, plain)
        setm(msg_off(i), '%s: OFF' % label, plain)

def sarc_read(d):
    hl = struct.unpack_from('<H', d, 4)[0]; doff = struct.unpack_from('<I', d, 0xc)[0]
    nn, mult = struct.unpack_from('<HI', d, hl + 6)
    sfnt = hl + 0xc + nn * 16
    files = []
    for i in range(nn):
        h, attr, s, e = struct.unpack_from('<IIII', d, hl + 0xc + i * 16)
        name = d[sfnt + 8 + (attr & 0xffff) * 4:].split(b'\0')[0].decode()
        files.append((name, d[doff + s:doff + e]))
    return mult, files

def sarc_hash(name, mult):
    h = 0
    for c in name.encode():
        h = (h * mult + c) & 0xffffffff
    return h

def sarc_write(mult, files, align=0x100):
    files = sorted(files, key=lambda f: sarc_hash(f[0], mult))
    names = bytearray(); name_off = []
    for n, _ in files:
        name_off.append(len(names) // 4); names += n.encode() + b'\0'; names += b'\0' * (-len(names) % 4)
    sfat = bytearray(b'SFAT' + struct.pack('<HHI', 0xc, len(files), mult))
    data = bytearray(); ranges = []
    for _, blob in files:
        data += b'\0' * (-len(data) % align); ranges.append((len(data), len(data) + len(blob))); data += blob
    for (n, _), no, (s, e) in zip(files, name_off, ranges):
        sfat += struct.pack('<IIII', sarc_hash(n, mult), 0x01000000 | no, s, e)
    sfnt = b'SFNT' + struct.pack('<HH', 8, 0) + names
    head_len = 0x14 + len(sfat) + len(sfnt)
    doff = (head_len + align - 1) // align * align
    total = doff + len(data)
    hdr = b'SARC' + struct.pack('<HHIIHH', 0x14, 0xfeff, total, doff, 0x100, 0)
    return hdr + sfat + sfnt + b'\0' * (doff - head_len) + data

def build_messages(lang):
    src = os.path.join(ROMFS, 'Message', 'TalkSys_%s.sarc.zs' % lang)
    d = zstandard.ZstdDecompressor().stream_reader(open(src, 'rb').read()).read()
    mult, files = sarc_read(d)
    out_files = []
    for name, blob in files:
        if name == 'SYS_PhoneApp.msbt':
            header, ng, asize, entries = msbt_read(blob)
            patch_messages(entries)
            blob = msbt_write(header, ng, asize, entries)
            # sanity: re-read
            assert len(msbt_read(blob)[3]) == len(entries)
        out_files.append((name, blob))
    sarc = sarc_write(mult, out_files)
    dst = os.path.join(OUT, 'Message', 'TalkSys_%s.sarc.zs' % lang)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(zstandard.ZstdCompressor(level=19).compress(sarc))
    print('wrote', dst)

# ---------------------------------------------------------------- event flow
def build_flow():
    ef = evfl.EventFlow(); ef.read(open(BASE_FLOW, 'rb').read())
    fc = ef.flowchart
    ev = {e.name: e for e in fc.events}
    actors = {a.identifier.name: a for a in fc.actors}
    sysa, player = actors['EventFlowSystemActor'], actors['Player']
    def act_of(a, name): return [x for x in a.actions if x.v == name][0]
    def qry_of(a, name): return [x for x in a.queries if x.v == name][0]
    counter = [0]

    def new_event(data):
        e = evfl.event.Event()
        counter[0] += 1
        e.name = 'SLT_Event%d' % counter[0]
        e.data = data
        fc.events.append(e)
        return e

    def params(d):
        c = Container(); c.data.update(d); return c

    def message(label, nxt=None):
        a = evfl.event.ActionEvent()
        a.actor.v = player; a.actor_action.v = act_of(player, 'EventFlowActionOpenMessageWindow')
        a.params = params({'MessageID': 'TalkSys/SYS_PhoneApp:%s' % label, 'IsCloseMessageWindow': False})
        a.nxt.v = nxt
        return new_event(a)

    def flag_action(name, key, nxt):
        a = evfl.event.ActionEvent()
        a.actor.v = sysa; a.actor_action.v = act_of(sysa, name)
        a.params = params({'AccessKey': 'cLand:SLT_%s' % key})
        a.nxt.v = nxt
        return new_event(a)

    def switch(query, cases, p=None):
        s = evfl.event.SwitchEvent()
        s.actor.v = sysa; s.actor_query.v = qry_of(sysa, query)
        s.params = params(p) if p else None
        for k, target in cases.items():
            s.cases[k] = evfl.util.make_rindex(target)
        return new_event(s)

    # Menus first (targets filled in afterwards because the toggles loop back to them).
    page_msgs = [message(msg_page(p)) for p in range(len(PAGES))]
    feature_index = {k: i for i, (k, _) in enumerate(FEATURES)}

    def toggle(key, back):
        on_msg = message(msg_on(feature_index[key]), back)
        off_msg = message(msg_off(feature_index[key]), back)
        turn_on = flag_action('EventFlowActionEventFlagSystemOn', key, on_msg)
        turn_off = flag_action('EventFlowActionEventFlagSystemOff', key, off_msg)
        # EventFlowQueryEventFlagSystemBool: case 0 = flag is set, case 1 = not set
        # (same as the game's own "cPlayer:EquipLicenseHelmet" check in this flow).
        return switch('EventFlowQueryEventFlagSystemBool', {0: turn_off, 1: turn_on}, {'AccessKey': 'cLand:SLT_%s' % key})

    for p, page in enumerate(PAGES):
        cases = {i: toggle(k, page_msgs[p]) for i, (k, _) in enumerate(page)}
        n = len(page)
        if p < len(PAGES) - 1:
            cases[n] = page_msgs[p + 1]; n += 1          # More settings...
        if p > 0:
            cases[n] = page_msgs[p - 1]; n += 1          # Back
        n += 1                                           # last choice: done -> end
        page_msgs[p].data.nxt.v = switch('EventFlowQueryGeneralTalkChoice%d' % n, cases)
    menu1_msg = page_msgs[0]

    # The Slumber phone app starts this flow at "SlumberApp" (the module swaps the entry name).
    ep = evfl.entry_point.EntryPoint('SlumberApp')
    ep.main_event = evfl.util.make_index(menu1_msg)
    fc.entry_points.append(ep)

    buf = io.BytesIO(); ef.write(buf)
    dst = os.path.join(OUT, 'EventFlow', 'Player_ConstructionLicenseApp.bfevfl')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(buf.getvalue())
    print('wrote', dst, len(buf.getvalue()), 'bytes,', counter[0], 'new events')

# ---------------------------------------------------------------- resource size table
# The game reserves a fixed buffer per file (System/Resource/ResourceSizeTable.srsizetable, Yaz0 "RSTC":
# 12-byte header, then {crc32(path), size, flags}). A replacement bigger than its entry fails to load
# (the boot crashed on our bigger TalkSys archive), so raise the entries of the files we enlarge.
def build_rstb(sizes):
    sys.path.insert(0, HERE)
    import yaz0, zlib
    src = os.path.join(ROMFS, 'System', 'Resource', 'ResourceSizeTable.srsizetable')
    d = bytearray(yaz0.decompress(open(src, 'rb').read()))
    assert d[:4] == b'RSTC'
    ncrc = struct.unpack_from('<I', d, 4)[0]
    index = {struct.unpack_from('<I', d, 12 + i * 12)[0]: 12 + i * 12 for i in range(ncrc)}
    for path, size in sizes.items():
        crc = zlib.crc32(path.encode())
        if crc not in index:   # a new file (the bridge / incline pictures): insert, keeping the crcs sorted
            new = (size + 0x2500 + 0x1f) & ~0x1f
            pos = 12
            while pos < 12 + ncrc * 12 and struct.unpack_from('<I', d, pos)[0] < crc: pos += 12
            d[pos:pos] = struct.pack('<III', crc, new, 0)
            ncrc += 1
            struct.pack_into('<I', d, 4, ncrc)
            index = {struct.unpack_from('<I', d, 12 + i * 12)[0]: 12 + i * 12 for i in range(ncrc)}
            print('RSTB %s: new %d (file %d)' % (path, new, size))
            continue
        off = index[crc]
        old = struct.unpack_from('<I', d, off + 4)[0]
        new = max(old, (size + 0x400 + 0x1f) & ~0x1f)
        struct.pack_into('<I', d, off + 4, new)
        print('RSTB %s: %d -> %d (file %d)' % (path, old, new, size))
    dst = os.path.join(OUT, 'System', 'Resource', 'ResourceSizeTable.srsizetable')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    comp = yaz0.compress(bytes(d))
    assert yaz0.decompress(comp) == bytes(d)
    open(dst, 'wb').write(comp)
    print('wrote', dst, len(comp))

def decompressed_size(path):
    return len(zstandard.ZstdDecompressor().stream_reader(open(path, 'rb').read()).read())

# ---------------------------------------------------------------- phone app name
def build_layout_messages(lang, base=None, out=None):
    src = os.path.join(base or ROMFS, 'Message', 'LayoutMsg_%s.sarc.zs' % lang)
    d = zstandard.ZstdDecompressor().stream_reader(open(src, 'rb').read()).read()
    mult, files = sarc_read(d)
    out_files = []
    for name, blob in files:
        if name == 'MenuDevice.msbt':
            header, ng, asize, entries = msbt_read(blob)
            for label, text in APP_NAMES:
                if not any(e[0] == label for e in entries):
                    pos = next(i for i, e in enumerate(entries) if e[0] > label)
                    entries.insert(pos, [label, b'', u16(text)])
            blob = msbt_write(header, ng, asize, entries)
            assert all(any(e[0] == label for e in msbt_read(blob)[3]) for label, _ in APP_NAMES)
        out_files.append((name, blob))
    dst = os.path.join(out or OUT, 'Message', 'LayoutMsg_%s.sarc.zs' % lang)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(zstandard.ZstdCompressor(level=19).compress(sarc_write(mult, out_files)))
    print('wrote', dst)

# ---------------------------------------------------------------- phone app icon
# LMenuDeviceBtn: the icon goes over DeviceIconBook^x (only shown by "Type" frame 4, which no app uses).
# A phone button has two versions: N_NonSelect_00 (static tile + P_Icon_00, whose texture the "Type"
# frame picks) and N_Select_00 with one group per app that fades in while the button is highlighted.
# Frame 4's highlighted group is N_Book (the old Critterpedia: orange tile P_Base_Book + two critter
# sprites P_Book_00/01, two-tone masks shared with the current Critterpedia). So:
#   - Type frame 4: static tile purple (P_Base_00 colour channels 4/5/6)
#   - layout: add DeviceIconBook^x to the texture list; P_Book_00 shows it full colour (material
#     black 0 / white 255 like P_Icon_00), centred and icon-sized; P_Book_01 hidden; P_Base_Book purple
#   - SelectLoop: P_Base_Book's colour pulse moved from orange to purple
ICON_TEXTURE = 'DeviceIconBook^x'
ICON_FRAME = 4.0
# Gyroid clay: a warm terracotta tile under the pixel-art gyroid (phone_icon.py).
TILE_COLOR = (214, 150, 92)      # P_Base_00 colour channels 4/5/6 at frame 4
SEL_TILE_BLACK = (214, 150, 92)  # P_Base_Book material colours
SEL_TILE_WHITE = (226, 164, 106)
# P_Book_00: SelectLoop bobs its Y between -67 and -58 (bottom anchor), so a 148x148 icon at 0.82
# anchored bottom-centre sits centred, about as big as the highlighted static icon.
SEL_ICON_SIZE, SEL_ICON_SCALE, SEL_ICON_ORIGIN = 148.0, 0.82, 0x08

def flan_keys(body):
    """Yield (pane name, tag magic, target, key data offset, key count, data type) for a BFLAN."""
    hs = struct.unpack_from('<H', body, 6)[0]; ns = struct.unpack_from('<H', body, 0x10)[0]
    p = hs
    for _ in range(ns):
        mg = body[p:p + 4]; sz = struct.unpack_from('<I', body, p + 4)[0]
        if mg == b'pai1':
            nent = struct.unpack_from('<H', body, p + 0xe)[0]
            entoff = struct.unpack_from('<I', body, p + 0x10)[0]
            for eo in struct.unpack_from('<%dI' % nent, body, p + entoff):
                e = p + eo
                name = body[e:e + 28].split(b'\0')[0].decode(); ntag = body[e + 28]
                for to in struct.unpack_from('<%dI' % ntag, body, e + 32):
                    t = e + to; tmg = body[t:t + 4].decode(); nsub = body[t + 4]
                    for so in struct.unpack_from('<%dI' % nsub, body, t + 8):
                        s = t + so
                        target, dtype = body[s + 1], body[s + 2]
                        nkey = struct.unpack_from('<H', body, s + 4)[0]
                        koff = struct.unpack_from('<I', body, s + 8)[0]
                        yield name, tmg, target, s + koff, nkey, dtype
        p += sz

def patch_type_anim(body):
    body = bytearray(body)
    done = []
    for name, tag, target, ko, n, dtype in list(flan_keys(body)):
        if name == 'P_Base_00' and tag == 'FLMC' and target in (4, 5, 6) and dtype == 2:
            colour = TILE_COLOR[target - 4]
            for k in range(n):                      # hermite keys (frame, value, slope); the segment
                fr, val, sl = struct.unpack_from('<fff', body, ko + k * 12)   # [4, 5) is keys k..k+1
                if fr == ICON_FRAME and k + 1 < n and struct.unpack_from('<f', body, ko + (k + 1) * 12)[0] == ICON_FRAME + 1:
                    struct.pack_into('<f', body, ko + k * 12 + 4, float(colour))
                    struct.pack_into('<f', body, ko + (k + 1) * 12 + 4, float(colour))
                    done.append('tile %d' % target)
    assert len(done) == 3, done
    return bytes(body)

def patch_select_loop(body):
    body = bytearray(body)
    n_keys = 0
    for name, tag, target, ko, n, dtype in list(flan_keys(body)):
        if name == 'P_Base_Book' and tag == 'FLMC' and target == 0 and dtype == 2:
            for k in range(n):                      # black colour R pulses 233 <-> 237
                v = struct.unpack_from('<f', body, ko + k * 12 + 4)[0]
                struct.pack_into('<f', body, ko + k * 12 + 4, SEL_TILE_BLACK[0] + (v - 233.0))
                n_keys += 1
    assert n_keys > 0
    return bytes(body)

def flyt_sections(body):
    p = struct.unpack_from('<H', body, 6)[0]; cnt = struct.unpack_from('<H', body, 0x10)[0]
    for _ in range(cnt):
        sz = struct.unpack_from('<I', body, p + 4)[0]
        yield bytes(body[p:p + 4]), p, sz
        p += sz

def patch_layout(body):
    body = bytearray(body)
    # 1. texture list: append ICON_TEXTURE (txl1: count u16, pad, u32 offsets from the table start, names)
    t, s0, sz = [x for x in flyt_sections(body) if x[0] == b'txl1'][0]
    count = struct.unpack_from('<H', body, s0 + 8)[0]
    offs = list(struct.unpack_from('<%dI' % count, body, s0 + 0xc))
    names = [bytes(body[s0 + 0xc + o:body.index(b'\0', s0 + 0xc + o)]).decode() for o in offs]
    assert ICON_TEXTURE not in names
    names.append(ICON_TEXTURE)
    pool = bytearray(); new_offs = []
    for nme in names:
        new_offs.append(4 * len(names) + len(pool)); pool += nme.encode() + b'\0'
    data = struct.pack('<HH', len(names), 0) + struct.pack('<%dI' % len(names), *new_offs) + pool
    data += b'\0' * (-(8 + len(data)) % 4)
    sec = b'txl1' + struct.pack('<I', 8 + len(data)) + data
    body[s0:s0 + sz] = sec
    struct.pack_into('<I', body, 0xc, len(body))
    tex_index = len(names) - 1
    # 2. materials and panes
    mats = {}; panes = {}
    for t, p, sz in flyt_sections(body):
        if t == b'mat1':
            n = struct.unpack_from('<H', body, p + 8)[0]
            for o in struct.unpack_from('<%dI' % n, body, p + 0xc):
                mats[bytes(body[p + o:p + o + 0x1c]).split(b'\0')[0].decode()] = p + o
        if t in (b'pic1', b'pan1'):
            panes[bytes(body[p + 0xc:p + 0x24]).split(b'\0')[0].decode()] = p
    # material v9: name[0x1c], flags u32, colour offsets u32, black RGBA8, white RGBA8, texmaps (u16 index, wrap, wrap)
    m = mats['P_Book_00']
    assert struct.unpack_from('<I', body, m + 0x1c)[0] & 3 == 1
    struct.pack_into('<H', body, m + 0x2c, tex_index)
    body[m + 0x24:m + 0x2c] = bytes([0, 0, 0, 0, 255, 255, 255, 255])
    m = mats['P_Base_Book']
    body[m + 0x24:m + 0x27] = bytes(SEL_TILE_BLACK); body[m + 0x28:m + 0x2b] = bytes(SEL_TILE_WHITE)
    # pane: flags, origin, alpha, ..., name[24], user[8], translate xyz, rotate xyz, scale xy, size wh
    p = panes['P_Book_00']
    body[p + 9] = SEL_ICON_ORIGIN
    struct.pack_into('<ff', body, p + 0x2c, 0.0, -67.0)
    struct.pack_into('<ff', body, p + 0x44, SEL_ICON_SCALE, SEL_ICON_SCALE)
    struct.pack_into('<ff', body, p + 0x4c, SEL_ICON_SIZE, SEL_ICON_SIZE)
    body[panes['P_Book_01'] + 8] &= ~1                       # not visible
    return bytes(body)

def sarc_rebuild(buf, replace):
    """Rebuild a SARC with some files replaced, keeping every file's original alignment."""
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

def sarc_files(buf):
    hl = struct.unpack_from('<H', buf, 4)[0]; doff = struct.unpack_from('<I', buf, 0xc)[0]
    nn = struct.unpack_from('<H', buf, hl + 6)[0]
    sfnt = hl + 0xc + nn * 16
    for i in range(nn):
        h, attr, s, e = struct.unpack_from('<IIII', buf, hl + 0xc + i * 16)
        yield bytes(buf[sfnt + 8 + (attr & 0xffff) * 4:]).split(b'\0')[0].decode(), bytes(buf[doff + s:doff + e])

def patch_icon_texture(bntx):
    sys.path.insert(0, HERE)
    import phone_icon, swz
    from bntx import textures
    t = [t for t in textures(bntx) if t['name'] == ICON_TEXTURE][0]
    assert t['fmt'] == 0x2e06 and (t['w'], t['h']) == (180, 180), t
    data, bw, bh = phone_icon.encode_image(phone_icon.draw_icon())
    out = bytearray(bntx)
    region = bytearray(bntx[t['data']:t['data'] + t['size']])
    swz.swizzle_into(region, data, bw, bh, 16, t['bh'])
    out[t['data']:t['data'] + t['size']] = region
    return bytes(out)

def build_phone_layout(base=None, out_dir=None):
    src = os.path.join(base or ROMFS, 'Layout', 'LMenuDeviceBtn.Nin_NX_NVN.zs')
    d = zstandard.ZstdDecompressor().stream_reader(open(src, 'rb').read()).read()
    (arc_name, arc), = list(sarc_files(d))          # nested SARC: one .arc holding the layout files
    inner = dict(sarc_files(arc))
    patched = {
        'anim/LMenuDeviceBtn_Type.bflan': patch_type_anim(inner['anim/LMenuDeviceBtn_Type.bflan']),
        'anim/LMenuDeviceBtn_SelectLoop.bflan': patch_select_loop(inner['anim/LMenuDeviceBtn_SelectLoop.bflan']),
        'timg/__Combined.bntx': patch_icon_texture(inner['timg/__Combined.bntx']),
        'blyt/LMenuDeviceBtn.bflyt': patch_layout(inner['blyt/LMenuDeviceBtn.bflyt']),
    }
    new_arc = sarc_rebuild(arc, patched)
    assert dict(sarc_files(new_arc))['blyt/LMenuDeviceBtn.bflyt'] == patched['blyt/LMenuDeviceBtn.bflyt']
    out = sarc_rebuild(d, {arc_name: new_arc})
    dst = os.path.join(out_dir or OUT, 'Layout', 'LMenuDeviceBtn.Nin_NX_NVN.zs')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(zstandard.ZstdCompressor(level=19).compress(out))
    print('wrote', dst, len(d), '->', len(out))
    return len(out)

build_messages('USen')
build_messages('EUen')
build_layout_messages('USen')
build_layout_messages('EUen')
build_flow()
sys.path.insert(0, HERE)
import stage_hud
_stage, _ = stage_hud.add_hud(open(os.path.join(ROMFS, 'Stage', 'Game', 'MainField.byml'), 'rb').read())
os.makedirs(os.path.join(OUT, 'Stage', 'Game'), exist_ok=True)
open(os.path.join(OUT, 'Stage', 'Game', 'MainField.byml'), 'wb').write(_stage)
print('wrote', os.path.join(OUT, 'Stage', 'Game', 'MainField.byml'), len(_stage))
phone_layout_size = build_phone_layout()
layout_msg_size = {lang: decompressed_size(os.path.join(OUT, 'Message', 'LayoutMsg_%s.sarc.zs' % lang)) for lang in ('USen', 'EUen')}
if ADEN_ROMFS:
    phone_layout_size = max(phone_layout_size, build_phone_layout(ADEN_ROMFS, ADEN_OUT))
    for lang in ('USen', 'EUen'):
        build_layout_messages(lang, ADEN_ROMFS, ADEN_OUT)
        layout_msg_size[lang] = max(layout_msg_size[lang], decompressed_size(os.path.join(ADEN_OUT, 'Message', 'LayoutMsg_%s.sarc.zs' % lang)))
import bridges_tab   # the Bridges tab of the HHP house menu (island editor)
bridges_sizes = bridges_tab.build(ROMFS, OUT, sarc_files, sarc_rebuild, sarc_read, sarc_write, msbt_read, msbt_write)
import dpad_icon     # the D-pad left (spawn) icon in the island editor's D-pad guide
bridges_sizes.update(dpad_icon.build(ROMFS, OUT, sarc_files, sarc_rebuild))
build_rstb(dict(bridges_sizes, **{
    'Layout/LMenuDeviceBtn.Nin_NX_NVN': phone_layout_size,
    'Message/TalkSys_USen.sarc': decompressed_size(os.path.join(OUT, 'Message', 'TalkSys_USen.sarc.zs')),
    'Message/TalkSys_EUen.sarc': decompressed_size(os.path.join(OUT, 'Message', 'TalkSys_EUen.sarc.zs')),
    'Message/LayoutMsg_USen.sarc': layout_msg_size['USen'],
    'Message/LayoutMsg_EUen.sarc': layout_msg_size['EUen'],
    'EventFlow/Player_ConstructionLicenseApp.bfevfl': os.path.getsize(os.path.join(OUT, 'EventFlow', 'Player_ConstructionLicenseApp.bfevfl')),
}))
