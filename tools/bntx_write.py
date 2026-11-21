# BNTX writer for small UI texture archives (like a layout's timg/__Combined.bntx): rebuilds the
# whole file from its textures, so a texture can be added. Keeps the layout Nintendo's tools use
# (header, memory pool, BRTI pointer array, _STR + _DIC, BRTI blocks, BRTD, _RLT). Rebuilding the HHP
# house menu's tab-button archive unchanged gives the same bytes up to the relocation table, and the
# table relocates exactly the same pointers (it is just encoded with more, shorter runs). Only single-mip, single-layer 2D textures are supported, which is what UI archives hold.
import struct
from bntx import textures
import resdic

BRTI_SIZE = 0x2a8    # BRTI block: 0xa0 info + 0x100 texture + 0x100 view + 8 (one mip offset)


def read(d):
    """-> dict(file_name, string_order, textures=[dict(name, brti (raw 0xa0 bytes), data)])"""
    out = []
    for t in textures(d):
        assert t['mips'] == 1, t['name']
        out.append(dict(name=t['name'], brti=bytes(d[t['brti']:t['brti'] + 0xa0]), data=bytes(d[t['data']:t['data'] + t['size']]),
                        align=t['align']))
    name_ptr = struct.unpack_from('<I', d, 0x10)[0]
    file_name = d[name_ptr:d.index(b'\0', name_ptr)].decode()
    # string pool order (kept so a plain rebuild is identical)
    s0 = d.index(b'_STR')
    n = struct.unpack_from('<I', d, s0 + 0x10)[0]
    p = s0 + 0x18
    order = []
    for _ in range(n):
        ln = struct.unpack_from('<H', d, p)[0]
        order.append(d[p + 2:p + 2 + ln].decode())
        p += 2 + ln + 1
        p += p & 1
    return dict(file_name=file_name, string_order=order, textures=out, header=bytes(d[:0x58]))


def write(arc):
    texs = sorted(arc['textures'], key=lambda t: t['name'].encode())
    n = len(texs)
    order = [s for s in arc['string_order'] if s == arc['file_name'] or any(t['name'] == s for t in texs)]
    order += [t['name'] for t in texs if t['name'] not in order]
    relocs0, relocs1 = [], []          # pointer positions: section 0 (file), section 1 (BRTD)
    out = bytearray(0x58 + 0x140)      # header + NX header, memory pool
    ptr_arr = len(out); out += b'\0' * 8 * n
    str_blk = len(out)
    out += b'_STR' + b'\0' * 12
    out += struct.pack('<I', len(order))
    str_pos = {}
    str_pos[''] = len(out); out += b'\0\0\0\0'       # the dictionary root's empty name
    for s in order:
        str_pos[s] = len(out)
        b = s.encode()
        out += struct.pack('<H', len(b)) + b + b'\0'
        if len(out) & 1: out += b'\0'
    out += b'\0' * (-len(out) % 8)
    dic = len(out)
    nodes = resdic.build([t['name'].encode() for t in texs])
    out += b'_DIC' + struct.pack('<I', n)
    for ref, l, r, key in nodes:
        relocs0.append(len(out) + 8)
        out += struct.pack('<iHHQ', ref, l, r, str_pos[key.decode()])
    struct.pack_into('<II', out, str_blk + 4, len(out) - str_blk, len(out) - str_blk)
    brti_at = []
    for t in texs:
        b = len(out); brti_at.append(b)
        info = bytearray(t['brti'])
        struct.pack_into('<II', info, 4, BRTI_SIZE, BRTI_SIZE)
        struct.pack_into('<QQQ', info, 0x60, str_pos[t['name']], 0x20, b + 0x2a0)
        struct.pack_into('<QQ', info, 0x80, b + 0xa0, b + 0x1a0)
        out += info + b'\0' * (0x2a0 - 0xa0) + b'\0' * 8
        relocs0 += [b + 0x60, b + 0x68, b + 0x70, b + 0x80, b + 0x88]
        relocs1.append(b + 0x2a0)
    sec0_size = len(out)
    align = 1 << arc['header'][0xe]
    data_at = (len(out) + 0x10 + align - 1) // align * align
    brtd = data_at - 0x10
    out += b'\0' * (brtd - len(out))
    last = brti_at[-1]
    struct.pack_into('<II', out, last + 4, brtd - last, brtd - last)   # last BRTI block runs up to BRTD
    out += b'BRTD' + b'\0' * 12
    for t, b in zip(texs, brti_at):
        out += b'\0' * (-len(out) % t['align'])
        struct.pack_into('<Q', out, b + 0x2a0, len(out))
        struct.pack_into('<I', out, b + 0x50, len(t['data']))
        out += t['data']
    out += b'\0' * (-len(out) % align)
    struct.pack_into('<Q', out, brtd + 8, len(out) - brtd)
    rlt = len(out)
    # header
    out[:0x20] = arc['header'][:0x20]
    struct.pack_into('<I', out, 0x10, str_pos[arc['file_name']] + 2)
    out[0x20:0x58] = arc['header'][0x20:0x58]
    struct.pack_into('<I', out, 0x24, n)
    struct.pack_into('<QQQQ', out, 0x28, ptr_arr, brtd, dic, 0x58)
    struct.pack_into('<H', out, 0x16, str_blk)
    relocs0 += [0x28, 0x38, 0x40]
    relocs1 += [0x30]
    for i, b in enumerate(brti_at):
        struct.pack_into('<Q', out, ptr_arr + 8 * i, b)
        relocs0.append(ptr_arr + 8 * i)
    # relocation table
    entries0, entries1 = encode(sorted(relocs0)), encode(sorted(relocs1))
    t = bytearray(b'_RLT' + struct.pack('<III', rlt, 2, 0))
    t += struct.pack('<QIIII', 0, 0, sec0_size, 0, len(entries0))
    t += struct.pack('<QIIII', 0, brtd, len(out) - brtd, len(entries0), len(entries1))
    for e in entries0 + entries1: t += struct.pack('<IHBB', *e)
    out += t
    struct.pack_into('<II', out, 0x18, rlt, len(out))
    return bytes(out)


def encode(ptrs):
    """Pointer positions -> _RLT entries (offset, struct count, pointers per struct, u64s skipped
    after each), merging runs the way Nintendo's encoder does: the first gap after a run of
    consecutive pointers becomes the stride if the next run repeats it."""
    entries = []
    i = 0
    while i < len(ptrs):
        start = ptrs[i]
        k = 1
        while i + k < len(ptrs) and ptrs[i + k] == start + 8 * k and k < 255: k += 1
        # try to repeat (k pointers, then a gap) as structs
        structs, pad = 1, 0
        if i + k < len(ptrs):
            gap = (ptrs[i + k] - (start + 8 * k)) // 8
            if 0 < gap < 256 and (ptrs[i + k] - start) % 8 == 0:
                stride = 8 * (k + gap)
                s = 1
                while s < 0xffff:
                    base = start + stride * s
                    idx = i + k * s
                    if idx + k > len(ptrs) or ptrs[idx:idx + k] != [base + 8 * j for j in range(k)]: break
                    s += 1
                if s > 1: structs, pad = s, gap
        entries.append((start, structs, k, pad))
        i += structs * k
    return entries
