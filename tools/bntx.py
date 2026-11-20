# Minimal BNTX reader: lists BRTI textures (name, format, size, mips, block height, data offsets).
import struct, sys
FORMATS = {0x0b01:'RGBA8', 0x0b06:'RGBA8_SRGB', 0x1a01:'BC1', 0x1a06:'BC1_SRGB', 0x1b01:'BC2', 0x1b06:'BC2_SRGB',
           0x1c01:'BC3', 0x1c06:'BC3_SRGB', 0x1d01:'BC4', 0x1d02:'BC4_S', 0x1e01:'BC5', 0x1e02:'BC5_S',
           0x2001:'BC7', 0x2006:'BC7_SRGB', 0x0201:'R8', 0x0901:'RG8', 0x0701:'RGB565', 0x0801:'RGB5A1'}
def textures(d):
    assert d[:4] == b'BNTX'
    count = struct.unpack_from('<I', d, 0x24)[0]
    infoptrs = struct.unpack_from('<Q', d, 0x28)[0]
    out = []
    for i in range(count):
        bo = struct.unpack_from('<Q', d, infoptrs + 8 * i)[0]
        assert d[bo:bo+4] == b'BRTI', hex(bo)
        flags, dim, tile, swz, mips, samples = struct.unpack_from('<BBHHHI', d, bo + 0x10)
        fmt, access, w, h, depth, arr, layout, layout2 = struct.unpack_from('<IIiiiiII', d, bo + 0x1c)
        imgsize, align, compsel, ttype = struct.unpack_from('<IIII', d, bo + 0x50)
        name_addr, parent, ptrs = struct.unpack_from('<QQQ', d, bo + 0x60)
        nlen = struct.unpack_from('<H', d, name_addr)[0]
        name = d[name_addr+2:name_addr+2+nlen].decode()
        mipoffs = struct.unpack_from('<%dQ' % mips, d, ptrs)
        out.append(dict(name=name, brti=bo, fmt=fmt, fmtname=FORMATS.get(fmt, hex(fmt)), w=w, h=h, mips=mips,
                        bh=layout & 7, size=imgsize, align=align, compsel=compsel, data=mipoffs[0], mipoffs=mipoffs, tile=tile))
    return out
if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    for t in textures(d):
        print('%-34s %-10s %4dx%-4d mips=%d bh=%d size=%6d data=%x comp=%08x' % (t['name'], t['fmtname'], t['w'], t['h'], t['mips'], t['bh'], t['size'], t['data'], t['compsel']))
