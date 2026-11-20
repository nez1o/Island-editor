# Tegra X1 block-linear swizzle for block-compressed textures (x, y in blocks; 16-byte blocks).
def div_up(a, b): return (a + b - 1) // b
def addr(x, y, width_blocks, bpp, block_height):
    gobs_w = div_up(width_blocks * bpp, 64)
    gob = ((y // (8 * block_height)) * 512 * block_height * gobs_w
           + (x * bpp // 64) * 512 * block_height
           + (y % (8 * block_height) // 8) * 512)
    xb = x * bpp
    return gob + ((xb % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((xb % 32) // 16) * 32 + (y % 2) * 16 + (xb % 16)
def deswizzle(data, bw, bh_blocks, bpp, bhlog2):
    out = bytearray(bw * bh_blocks * bpp)
    for y in range(bh_blocks):
        for x in range(bw):
            a = addr(x, y, bw, bpp, 1 << bhlog2)
            o = (y * bw + x) * bpp
            out[o:o+bpp] = data[a:a+bpp]
    return bytes(out)
def swizzle_into(buf, linear, bw, bh_blocks, bpp, bhlog2):
    for y in range(bh_blocks):
        for x in range(bw):
            a = addr(x, y, bw, bpp, 1 << bhlog2)
            o = (y * bw + x) * bpp
            buf[a:a+bpp] = linear[o:o+bpp]
