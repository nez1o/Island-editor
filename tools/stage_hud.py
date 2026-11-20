# Adds the Slumber HUD (resident UI scene CloudIslandModeChange: the top-right fence/helper/bag + "+"
# icons) to the main island's stage. Each stage's byml lists its resident UI scenes under key 2ccc87db
# as [{691a4d18: mmh3(scene name)}, ...]; Slumber Island lists CloudIslandModeChange there, the main
# island (MainField) does not. The new list and its new element are appended at the end of the file and
# the root entry is pointed at the new list, so nothing else moves.
# usage: stage_hud.py <RomFS> <out romfs>
import os, struct, sys

def mmh3(data, seed=0):
    c1, c2 = 0xcc9e2d51, 0x1b873593
    h = seed; n = len(data); r = n & ~3
    for i in range(0, r, 4):
        k = int.from_bytes(data[i:i + 4], 'little')
        k = (k * c1) & 0xffffffff; k = ((k << 15) | (k >> 17)) & 0xffffffff; k = (k * c2) & 0xffffffff
        h ^= k; h = ((h << 13) | (h >> 19)) & 0xffffffff; h = (h * 5 + 0xe6546b64) & 0xffffffff
    t = data[r:]; k = 0
    if len(t) >= 3: k ^= t[2] << 16
    if len(t) >= 2: k ^= t[1] << 8
    if t:
        k ^= t[0]; k = (k * c1) & 0xffffffff; k = ((k << 15) | (k >> 17)) & 0xffffffff
        h ^= (k * c2) & 0xffffffff
    h ^= n; h ^= h >> 16; h = (h * 0x85ebca6b) & 0xffffffff; h ^= h >> 13
    h = (h * 0xc2b2ae35) & 0xffffffff; return h ^ (h >> 16)

UI_KEY, NAME_KEY = '2ccc87db', '691a4d18'
SCENE = mmh3(b'CloudIslandModeChange')
assert SCENE == 0xf3792d3e

def keys_of(d):
    off = struct.unpack_from('<I', d, 4)[0]
    n = struct.unpack_from('<I', d, off)[0] >> 8
    offs = struct.unpack_from('<%dI' % (n + 1), d, off + 4)
    return [d[off + offs[i]: d.index(b'\0', off + offs[i])].decode() for i in range(n)]

def add_hud(d):
    d = bytearray(d)
    assert d[:2] == b'YB'
    keys = keys_of(d)
    root = struct.unpack_from('<I', d, 12)[0]
    assert d[root] == 0xc1
    for i in range(struct.unpack_from('<I', d, root)[0] >> 8):
        e = root + 4 + 8 * i
        if keys[struct.unpack_from('<I', d, e)[0] & 0xffffff] == UI_KEY: break
    else: raise KeyError(UI_KEY)
    assert d[e + 3] == 0xc0
    arr = struct.unpack_from('<I', d, e + 4)[0]
    n = struct.unpack_from('<I', d, arr)[0] >> 8
    types = bytes(d[arr + 4: arr + 4 + n])
    vals = list(struct.unpack_from('<%dI' % n, d, arr + 4 + ((n + 3) & ~3)))
    names = []
    for v in vals:                                   # each element: {691a4d18: hash}
        assert d[v] == 0xc1 and struct.unpack_from('<I', d, v)[0] >> 8 == 1
        names.append(struct.unpack_from('<I', d, v + 8)[0])
    if SCENE in names: return bytes(d), False
    proto = vals[0]
    while len(d) % 4: d.append(0)
    elem = len(d)                                    # copy of element 0 with the new hash
    d += d[proto: proto + 8] + struct.pack('<I', SCENE)
    new = len(d)
    m = n + 1
    d += struct.pack('<I', 0xc0 | (m << 8)) + (types + b'\xc1').ljust((m + 3) & ~3, b'\0')
    d += struct.pack('<%dI' % m, *(vals + [elem]))
    struct.pack_into('<I', d, e + 4, new)
    return bytes(d), True

if __name__ == '__main__':
    src = os.path.join(sys.argv[1], 'Stage', 'Game', 'MainField.byml')
    dst = os.path.join(sys.argv[2], 'Stage', 'Game', 'MainField.byml')
    out, changed = add_hud(open(src, 'rb').read())
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(out)
    print('wrote', dst, len(out), 'bytes', '(HUD added)' if changed else '(already there)')
