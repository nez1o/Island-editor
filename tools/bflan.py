# Minimal BFLAN (ui2d layout animation) reader / writer: enough to add keys, entries and pattern
# textures to an animation and write it back. Other sections (pat1, ...) are kept as they are.
# A read + write of an unchanged file gives the same bytes (checked on the HHP house menu's
# CategoryNum and the tab button's Pattern animations).
import struct


def read(b):
    assert b[:4] == b'FLAN'
    hl = struct.unpack_from('<H', b, 6)[0]
    n = struct.unpack_from('<H', b, 0x10)[0]
    pos, secs = hl, []
    for _ in range(n):
        mg = bytes(b[pos:pos + 4]); sz = struct.unpack_from('<I', b, pos + 4)[0]
        secs.append([mg, parse_pai1(b[pos:pos + sz]) if mg == b'pai1' else bytes(b[pos:pos + sz])])
        pos += sz
    return dict(header=bytes(b[:hl]), sections=secs)


def parse_pai1(s):
    frames, loop, _p, ntex, nent, entoff = struct.unpack_from('<HBBHHI', s, 8)
    texs = []
    for i in range(ntex):
        o = 0x14 + struct.unpack_from('<I', s, 0x14 + 4 * i)[0]
        texs.append(bytes(s[o:s.index(b'\0', o)]).decode())
    entries = []
    for i in range(nent):
        eo = struct.unpack_from('<I', s, entoff + 4 * i)[0]
        e = s[eo:]
        name = bytes(e[:28]).split(b'\0')[0].decode()
        ntag, kind = e[28], e[29]
        tags = []
        for t in range(ntag):
            to = struct.unpack_from('<I', e, 0x20 + 4 * t)[0]
            tg = e[to:]
            tname = bytes(tg[:4]).decode(); ninfo = tg[4]
            infos = []
            for k in range(ninfo):
                io = struct.unpack_from('<I', tg, 8 + 4 * k)[0]
                inf = tg[io:]
                idx, target, curve, _a, cnt, _b, keyoff = struct.unpack_from('<BBBBHHI', inf, 0)
                keys = []
                for q in range(cnt):
                    if curve == 1:   # step: frame, u16 value
                        f, v, _c = struct.unpack_from('<fHH', inf, keyoff + 8 * q); keys.append((f, v))
                    else:            # hermite: frame, value, slope
                        keys.append(struct.unpack_from('<fff', inf, keyoff + 12 * q))
                infos.append(dict(index=idx, target=target, curve=curve, keys=keys))
            tags.append(dict(tag=tname, infos=infos))
        entries.append(dict(name=name, kind=kind, tags=tags))
    return dict(frames=frames, loop=loop, textures=texs, entries=entries)


def write_pai1(p):
    out = bytearray(b'pai1' + b'\0' * 4)
    ntex = len(p['textures'])
    out += struct.pack('<HBBHHI', p['frames'], p['loop'], 0, ntex, len(p['entries']), 0)
    # texture table
    tab = len(out)
    out += b'\0' * 4 * ntex
    for i, t in enumerate(p['textures']):
        struct.pack_into('<I', out, tab + 4 * i, len(out) - tab)
        out += t.encode() + b'\0'
    out += b'\0' * (-len(out) % 4)
    entoff = len(out)
    struct.pack_into('<I', out, 0x10, entoff)
    out += b'\0' * 4 * len(p['entries'])
    for i, e in enumerate(p['entries']):
        struct.pack_into('<I', out, entoff + 4 * i, len(out))
        eb = bytearray(e['name'].encode().ljust(28, b'\0') + bytes([len(e['tags']), e['kind'], 0, 0]))
        eb += b'\0' * 4 * len(e['tags'])
        for ti, t in enumerate(e['tags']):
            struct.pack_into('<I', eb, 0x20 + 4 * ti, len(eb))
            tb = bytearray(t['tag'].encode() + bytes([len(t['infos']), 0, 0, 0]))
            tb += b'\0' * 4 * len(t['infos'])
            for ii, inf in enumerate(t['infos']):
                struct.pack_into('<I', tb, 8 + 4 * ii, len(tb))
                ib = bytearray(struct.pack('<BBBBHHI', inf['index'], inf['target'], inf['curve'], 0, len(inf['keys']), 0, 0xc))
                for k in inf['keys']:
                    ib += struct.pack('<fHH', k[0], k[1], 0) if inf['curve'] == 1 else struct.pack('<fff', *k)
                tb += ib
            eb += tb
        out += eb
    struct.pack_into('<I', out, 4, len(out))
    return bytes(out)


def write(a):
    body = bytearray(a['header'])
    for mg, s in a['sections']:
        body += write_pai1(s) if mg == b'pai1' else s
    struct.pack_into('<I', body, 0xc, len(body))
    struct.pack_into('<H', body, 0x10, len(a['sections']))
    return bytes(body)
