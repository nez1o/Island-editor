# Generate source/program/patches.inc from the IPS patch list in %TEMP%\acnh\mkips.py.
# Each IPS group becomes one switchable feature: {name, {offset, original word, patched word}...}.
#
# "slumber_terraform" is different: its sites are the map-attribute-table loads
# ("adrp xN, GOT page; ldr xN, [xN, #0x9f8]") inside the terraforming functions. The module
# rewrites them at runtime to "adrp xN, <copy>; add xN, xN, #<copy>" so that only terraforming
# sees a copy of the table where the real island has the Slumber bit (32). The patched words
# depend on where the module is loaded, so they are left 0 here and filled in by main.cpp.
# usage: gen_patches.py <path to mkips.py dir> <out .inc>
import sys, os, struct, importlib.util

src_dir, out = sys.argv[1], sys.argv[2]
sys.path.insert(0, src_dir)
spec = importlib.util.spec_from_file_location('mkips', os.path.join(src_dir, 'mkips.py'))
mkips = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mkips)

# Feature order and the names shown to the player.
FEATURES = [
    ('slumber_dpad',            'Slumber catalog + time (D-pad)'),
    ('instant_bridges',         'Instant bridges/inclines'),
    ('trees_anywhere',          'Trees anywhere'),
    ('outdoor_rugs',            'Rugs outdoors'),
    ('outdoor_partition_walls', 'Partition walls outdoors'),
    ('level4_access',           'Level-4 cliffs access'),
    ('slumber_terraform',       'Slumber terraforming (experimental)'),
    ('terraform_near_trees',    'Terraform near trees/cliffs (experimental)'),
]
DEFAULT_OFF = {'slumber_terraform', 'terraform_near_trees'}

# Switches implemented by hooks in main.cpp instead of instruction words (no patch list).
HOOK_FEATURES = [
    ('slumber_weather',         'Slumber weather (rain) on your island'),
    ('slumber_phone_app',       'Slumber phone app'),
    ('fast_terraform',          'Faster terraforming (3x)'),
    ('slumber_hud',             'Slumber HUD + build modes (+)'),
    ('island_editor',           'Island editor phone app (HHP-style editing)'),
    ('unlock_all',              'Unlock all phone apps, construction permits and HHP tools'),
    ('cliff_stack',             'Cliff tool stacks a cliff on a flush edge (instant, like a save editor)'),
]

# Terraform error bits cleared before the tool turns the check result into an error. Bit N is
# row N of FieldLandMakingError.bcsv (rows 0-8 = UniqueID 7-15, rows 9-17 = UniqueID 28-36):
# bit 3 = ID 10 cliff-create near tree, bit 5 = ID 12 cliff-remove tree on top,
# bit 6 = ID 13 river near tree, bit 8 = ID 15 cliff-remove next to another cliff.
# Kept: buildings (IDs 9/14/30), water on the cliff (11), system error (bit 1), ladders/vines/falls.
NEAR_TREE_CLEAR = (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8)
# Callers of the result->error mapper FUN_71021edb8c: "add x0, sp, #N; bl 0x21edb8c"
# (cliff tool, water tool, path tool). The bitmask is at [sp + N + 0x20].
NEAR_TREE_SITES = [0x2387bf0, 0x239292c, 0x23ad70c]

# The field checker loops over the tiles around the target and calls the "tree on this tile?"
# test 0x11ffd88; a hit jumps straight to an error and the tool gets no action at all (clearing
# the error bit later only hides the message). These "tbnz w0, #0, <error>" are made NOPs:
# 0x11f58a8 cliff create near tree (bit 3), 0x11f4678 river near tree (bit 6),
# 0x11f73f8 cliff remove with a tree on top (bit 5).
TREE_TEST_BRANCHES = [0x11f58a8, 0x11f4678, 0x11f73f8]

def near_tree_patches():
    img = open(mkips.MAIN, 'rb').read()
    out = []
    for a in TREE_TEST_BRANCHES:
        w = struct.unpack_from('<I', img, a)[0]
        assert (w & 0xfff8001f) == 0x37000000, hex(a)            # tbnz w0, #0, ...
        assert struct.unpack_from('<I', img, a - 4)[0] == 0x94000000 | (((0x11ffd88 - (a - 4)) >> 2) & 0x3ffffff), hex(a)
        out.append((a, w, 0xd503201f))
    return out

def mapper_cave_patches():          # v0.7 approach, unused (only hid the message)
    import cave_rug2, cave_bridge
    from cave_rug2 import b, find_gaps
    img = open(mkips.MAIN, 'rb').read()
    used, _ = cave_bridge.build(img, cave_bridge.rug_gaps(img))
    used = set(used)
    mask = (~NEAR_TREE_CLEAR) & 0xffffffff
    out = []
    for site in NEAR_TREE_SITES:
        add = struct.unpack_from('<I', img, site)[0]
        assert (add & 0xffc003ff) == 0x910003e0, hex(site)        # add x0, sp, #imm
        n = (add >> 10) & 0xfff
        assert struct.unpack_from('<I', img, site + 4)[0] & 0xfc000000 == 0x94000000
        off = n + 0x20
        ldr = 0xb9400000 | ((off // 4) << 10) | (31 << 5) | 9      # ldr w9, [sp, #off]
        movz = 0x52800000 | ((mask & 0xffff) << 5) | 10            # mov w10, #mask lo
        movk = 0x72a00000 | ((mask >> 16) << 5) | 10               # movk w10, #mask hi, lsl #16
        andw = 0x0a000000 | (10 << 16) | (9 << 5) | 9              # and w9, w9, w10
        strw = 0xb9000000 | ((off // 4) << 10) | (31 << 5) | 9     # str w9, [sp, #off]
        g = find_gaps(img, site, 3, used); used.update(g)
        code = {}
        def put(gap, ws):
            for k, w in enumerate(ws): code[gap + k * 4] = w
        put(g[0], [ldr, movz, b(g[0] + 8, g[1])])
        put(g[1], [movk, andw, b(g[1] + 8, g[2])])
        put(g[2], [strw, add, b(g[2] + 8, site + 4)])
        code[site] = b(site, g[0])
        for a in sorted(code):
            out.append((a, struct.unpack_from('<I', img, a)[0], code[a]))
    return out

# Terraforming functions that test the Slumber bit (32) of the per-map attribute table.
TERRAFORM_FUNCS = [0x21cc1b0, 0x21cdafc, 0x21cf570, 0x21d20bc, 0x21d2860, 0x21d5640, 0x21d6e20,
                   0x21d98e0, 0x21dbc40, 0x21dc440, 0x21de830, 0x21e04b0, 0x21e6490]
ATTR_GOT_PAGE, ATTR_GOT_OFF = 0x4ac3000, 0x9f8

# Whole field/terraforming code region (tree, cliff, water, path checks). Every table read in
# here sees the copy. Excluded: 0x21e8b40..0x21e9800, which only serves Slumber construction kits
# and would take a Slumber-only data path on the real island.
TERRAFORM_RANGE = (0x21aa000, 0x21f8000)
TERRAFORM_EXCLUDE = [(0x21e8b40, 0x21e9800)]

def terraform_sites():
    img = open(mkips.MAIN, 'rb').read()
    W = struct.unpack_from('<%dI' % (0x3f06df0 // 4), img, 0)
    sites = {}
    if True:
        lo, hi = TERRAFORM_RANGE
        for i in range(lo // 4, hi // 4):
            if any(a <= i * 4 < b for a, b in TERRAFORM_EXCLUDE): continue
            w = W[i]
            if (w & 0x9f000000) != 0x90000000: continue
            imm = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2)
            if imm & (1 << 20): imm -= 1 << 21
            if ((i * 4) & ~0xfff) + (imm << 12) != ATTR_GOT_PAGE: continue
            rd = w & 31
            for j in range(i + 1, i + 8):
                w2 = W[j]
                if (w2 & 0xffc00000) == 0xf9400000 and ((w2 >> 10) & 0xfff) * 8 == ATTR_GOT_OFF \
                   and ((w2 >> 5) & 31) == rd and (w2 & 31) == rd:
                    sites[i * 4] = (w, j * 4, w2)
                    break
    return sorted(sites.items())

lines = ['// Generated by tools/gen_patches.py from mkips.py - do not edit.', '']
total = 0
for key, _ in FEATURES:
    if key == 'slumber_terraform':
        sites = terraform_sites()
        lines.append('/* {adrp, ldr} pairs; patched words are computed at runtime (see main.cpp). */')
        lines.append('static PatchWord k_%s[] = {' % key)
        for adrp_off, (adrp_w, ldr_off, ldr_w) in sites:
            lines.append('    {0x%07x, 0x%08x, 0}, {0x%07x, 0x%08x, 0},' % (adrp_off, adrp_w, ldr_off, ldr_w))
        lines.append('};')
        total += 2 * len(sites)
        continue
    words = near_tree_patches() if key == 'terraform_near_trees' else mkips.PATCHES[key]
    lines.append('static const PatchWord k_%s[] = {' % key)
    for off, orig, new in words:
        lines.append('    {0x%07x, 0x%08x, 0x%08x},' % (off, orig, new))
    lines.append('};')
    total += len(words)
lines.append('')
lines.append('static Feature g_Features[] = {')
for key, label in FEATURES:
    on = 'false' if key in DEFAULT_OFF else 'true'
    lines.append('    {"%s", "%s", k_%s, sizeof(k_%s) / sizeof(PatchWord), %s, false},' % (key, label, key, key, on))
for key, label in HOOK_FEATURES:
    on = 'false' if key in DEFAULT_OFF else 'true'
    lines.append('    {"%s", "%s", nullptr, 0, %s, false},' % (key, label, on))
lines.append('};')
open(out, 'w', newline='\n').write('\n'.join(lines) + '\n')
print('wrote', out, total, 'patch words in', len(FEATURES), 'features +', len(HOOK_FEATURES), 'hook features')
