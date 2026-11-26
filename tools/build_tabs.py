# Rebuild only the island editor's menu files (Bridges / Inclines / Buildings tabs, D-pad icon) into an
# existing mod romfs, without running the whole build_romfs.py (which needs the evfl package and the
# phone-app inputs). Uses build_romfs.py's helpers and raises / adds entries in the romfs's own
# ResourceSizeTable, so the other files' entries stay as they are.
#
# usage: python3 tools/build_tabs.py <your RomFS dir> <mod romfs dir>
#   TAB_ICONS_ONLY=H   only rebuild the Buildings pictures (keeps the bridge / incline pictures,
#                      which may come from BRIDGE_PICTURES)
import os, struct, sys, types, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROMFS, OUT = sys.argv[1], sys.argv[2]

# build_romfs.py's helper functions (everything before its build steps)
src = open(os.path.join(HERE, 'build_romfs.py'), encoding='utf-8').read()
src = src[:src.index("build_messages('USen')")]
for m in ('evfl', 'evfl.common', 'evfl.container'):   # only the phone-app event flow uses it
    sys.modules.setdefault(m, types.ModuleType(m))
sys.modules['evfl.common'].StringHolder = object
sys.modules['evfl.container'].Container = object
helpers = {'__file__': os.path.join(HERE, 'build_romfs.py'), '__name__': 'build_romfs_helpers'}
saved = sys.argv
sys.argv = [saved[0], ROMFS, '-', OUT]
exec(compile(src, 'build_romfs.py', 'exec'), helpers)
sys.argv = saved

import bridges_tab, dpad_icon, yaz0
H = helpers
sizes = bridges_tab.build(ROMFS, OUT, H['sarc_files'], H['sarc_rebuild'], H['sarc_read'], H['sarc_write'],
                          H['msbt_read'], H['msbt_write'])
sizes.update(dpad_icon.build(ROMFS, OUT, H['sarc_files'], H['sarc_rebuild']))

# the size table: the mod's own copy (it already has this mod's other entries)
path = os.path.join(OUT, 'System', 'Resource', 'ResourceSizeTable.srsizetable')
d = bytearray(yaz0.decompress(open(path, 'rb').read()))
assert d[:4] == b'RSTC'
def index():
    n = struct.unpack_from('<I', d, 4)[0]
    return n, {struct.unpack_from('<I', d, 12 + i * 12)[0]: 12 + i * 12 for i in range(n)}
for name, size in sizes.items():
    crc = zlib.crc32(name.encode())
    n, idx = index()
    if crc not in idx:
        new = (size + 0x2500 + 0x1f) & ~0x1f
        pos = 12
        while pos < 12 + n * 12 and struct.unpack_from('<I', d, pos)[0] < crc: pos += 12
        d[pos:pos] = struct.pack('<III', crc, new, 0)
        struct.pack_into('<I', d, 4, n + 1)
        print('RSTB %s: new %d (file %d)' % (name, new, size))
        continue
    old = struct.unpack_from('<I', d, idx[crc] + 4)[0]
    new = max(old, (size + 0x400 + 0x1f) & ~0x1f)
    struct.pack_into('<I', d, idx[crc] + 4, new)
    if new != old: print('RSTB %s: %d -> %d (file %d)' % (name, old, new, size))
comp = yaz0.compress(bytes(d))
assert yaz0.decompress(comp) == bytes(d)
open(path, 'wb').write(comp)
print('done:', len(sizes), 'files')
